#!/usr/bin/env python3
"""Bake a Vulkan ``VK_EXT_opacity_micromap`` from a Wavefront OBJ + a material's
opacity texture, emitting the Aether opacity-micromap asset pair:

* ``<name>.omm``            — OBJ-style text sidecar (one record per base
                              triangle); the loader (Aether ``OmmImporter``)
                              packs it to ``VkMicromapTriangleKHR[]`` + the
                              LSB-first packed-state ``data`` buffer.
* ``<name>.micromap.toml``  — parameters + per-group usage histogram +
                              recorded sources (mesh + material) for staleness
                              ``--check``.

The on-disk representation is inspired by glTF ``EXT_mesh_opacity_micromap``
(bufferView-level binary) but adapted to Aether's TOML + OBJ-companion
conventions: TOML describes structure, a text sidecar holds the per-triangle
records, and a PNG opacity texture (referenced by the **material** via
``map_opacity``) supplies the per-texel opacity. The micromap is therefore the
bake product of ``(mesh UVs) x (material map_opacity)`` — the same dependency a
textured material already implies (its textures are authored against the mesh's
UV layout).

**The micromap is an accelerator, never a second definition of the cutout.**
The cut-out itself is OpenPBR's ``geometry_opacity`` driven by ``map_opacity``,
evaluated per hit by the renderers. The bake therefore samples the texture with
exactly the renderer's convention (wrap-repeat, **no V flip**, linear filtering)
and classifies each microtriangle *conservatively over its whole UV footprint*:
uniformly-transparent and uniformly-opaque microtriangles get the definitive
states, and anything straddling a cut-out edge gets an **unknown** state, which
hands that hit back to the shader's exact per-hit test. A micromapped mesh
therefore renders identically to the same mesh without one — only faster.

Microtriangle states are packed LSB-first within each byte in the recursive
space-filling-curve ordering defined by the Vulkan/glTF reference function
``BarycentricsToSpaceFillingCurveIndex`` (reproduced verbatim below).

Standard library only (``zlib`` + ``struct`` + ``tomllib``). Usage:

    # scene-driven (recommended): auto-discovers hero groups whose material
    # declares map_opacity, bakes them, and (with --write) injects the
    # opacity_micromaps reference into the scene's [[mesh]] entry.
    python tools/omm_bake.py --scene assets/shaderball_checker.scene.toml \\
        --mesh shader_ball --write

    # explicit: bake given groups from a named material's map_opacity
    python tools/omm_bake.py assets/shader_ball.obj \\
        --material-lib assets/shaderball_checker.materials.toml \\
        --material CheckerBall --groups BallSurface,BaseFoot

    # generate an 8x8-cell RGBA checkerboard opacity PNG
    python tools/omm_bake.py --emit-checker 256 256 8 8 assets/checker_opacity.png

    # re-bake from the recorded sources and diff (mesh x material drift check)
    python tools/omm_bake.py --check assets/shaderball_checker_omm.micromap.toml
"""

from __future__ import annotations

import argparse
import math
import os
import struct
import sys
import tempfile
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterator

try:
    import tomllib  # Python 3.11+
except ModuleNotFoundError as exc:  # pragma: no cover
    sys.stderr.write("error: omm_bake.py requires Python 3.11+ (tomllib)\n")
    raise SystemExit(2) from exc


# ── Constants ────────────────────────────────────────────────────────────────

FORMAT_TWO_STATE = 1  # 1 bit  / microtriangle: 0 transparent, 1 opaque
FORMAT_FOUR_STATE = 2  # 2 bits / microtriangle: 0/1/2/3 (trans/op/unk-trans/unk-op)

# Per-microtriangle state values (VK_EXT_opacity_micromap). The two "unknown"
# states are the *ask the shader* states: traversal hands the hit to the any-hit
# shader (Hyperion) / surfaces it as a RayQuery candidate (Theia) instead of
# deciding itself, and the suffix only names the fallback used when any-hit
# invocation is suppressed (e.g. a force-opaque ray).
STATE_TRANSPARENT = 0
STATE_OPAQUE = 1
STATE_UNKNOWN_TRANSPARENT = 2
STATE_UNKNOWN_OPAQUE = 3

# Per-triangle special indices (glTF/Vulkan): no micromap record is consumed.
SPECIAL_TRANSPARENT = -1
SPECIAL_OPAQUE = -2
SPECIAL_UNK_TRANSPARENT = -3
SPECIAL_UNK_OPAQUE = -4

U32 = 0xFFFFFFFF


# ── Number formatting (mirrors _obj_transform.fmt / obj_bounds.fmt) ──────────


def fmt(value: float) -> str:
    """Compact float formatting (``%.10g``), signed zero normalised."""
    if value == 0:
        value = 0.0
    return "%.10g" % value


# ── PNG decode / encode (standard library only) ──────────────────────────────


def decode_png(path: Path) -> tuple[int, int, int, bytes]:
    """Decode an 8-bit PNG to ``(width, height, channels, pixel_bytes)``.

    Supports colour types 0 (Gray), 2 (RGB), 4 (Gray+Alpha), 6 (RGBA). Indexed
    (type 3) and non-8-bit depths are rejected. All five PNG scanline filters
    (None/Sub/Up/Average/Paeth) are reconstructed. Raises ``ValueError`` on an
    unsupported or malformed file.
    """
    data = path.read_bytes()
    sig = b"\x89PNG\r\n\x1a\n"
    if data[:8] != sig:
        raise ValueError("%s: not a PNG (bad signature)" % path)
    pos = 8
    width = height = bit_depth = colour_type = None
    idat = bytearray()
    while pos < len(data):
        if pos + 8 > len(data):
            break
        (length,) = struct.unpack_from(">I", data, pos)
        pos += 4
        ctype = data[pos:pos + 4]
        pos += 4
        chunk = data[pos:pos + length]
        pos += length
        pos += 4  # CRC (not verified — zlib guards IDAT integrity)
        if ctype == b"IHDR":
            (width, height, bit_depth, colour_type, _comp, _filt, _interl) = struct.unpack(
                ">IIBBBBB", chunk
            )
        elif ctype == b"IDAT":
            idat.extend(chunk)
        elif ctype == b"IEND":
            break
    if width is None or bit_depth != 8:
        raise ValueError("%s: only 8-bit PNG supported" % path)
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(colour_type)
    if channels is None:
        raise ValueError("%s: unsupported PNG colour type %s" % (path, colour_type))

    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(stride * height)
    prev = bytearray(stride)
    rp = 0
    for y in range(height):
        if rp >= len(raw):
            break
        ftype = raw[rp]
        rp += 1
        row = bytearray(raw[rp:rp + stride])
        rp += stride
        if ftype == 0:
            pass
        elif ftype == 1:  # Sub
            for i in range(stride):
                left = row[i - channels] if i >= channels else 0
                row[i] = (row[i] + left) & 0xFF
        elif ftype == 2:  # Up
            for i in range(stride):
                row[i] = (row[i] + prev[i]) & 0xFF
        elif ftype == 3:  # Average
            for i in range(stride):
                left = row[i - channels] if i >= channels else 0
                row[i] = (row[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:  # Paeth
            for i in range(stride):
                left = row[i - channels] if i >= channels else 0
                up = prev[i]
                upleft = prev[i - channels] if i >= channels else 0
                p = left + up - upleft
                pa = abs(p - left)
                pb = abs(p - up)
                pc = abs(p - upleft)
                pred = left if (pa <= pb and pa <= pc) else (up if pb <= pc else upleft)
                row[i] = (row[i] + pred) & 0xFF
        else:
            raise ValueError("%s: unknown PNG filter %s" % (path, ftype))
        out[y * stride:(y + 1) * stride] = row
        prev = row
    return width, height, channels, bytes(out)


def encode_png_rgba(width: int, height: int, pixels: bytes) -> bytes:
    """Encode an 8-bit RGBA PNG (filter type 0 per scanline). ``pixels`` length
    must be ``width * height * 4``."""
    if len(pixels) != width * height * 4:
        raise ValueError("pixel buffer does not match width*height*4")

    def _chunk(ctype: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + ctype
            + payload
            + struct.pack(">I", zlib.crc32(ctype + payload) & U32)
        )

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    stride = width * 4
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter: None
        raw.extend(pixels[y * stride:(y + 1) * stride])
    idat = zlib.compress(bytes(raw), 9)
    return sig + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", idat) + _chunk(b"IEND", b"")


def make_checker_rgba(
    width: int, height: int, cells_u: int, cells_v: int
) -> bytes:
    """Build an RGBA checkerboard: opaque cells white (255,255,255,255),
    transparent cells black (0,0,0,0). Cell ``(i,j)`` is opaque when
    ``(i + j)`` is even. Cells tile evenly across the image."""
    if cells_u <= 0 or cells_v <= 0:
        raise ValueError("cells must be positive")
    out = bytearray(width * height * 4)
    cw = width / cells_u
    ch = height / cells_v
    for y in range(height):
        j = min(int(y // ch), cells_v - 1)
        for x in range(width):
            i = min(int(x // cw), cells_u - 1)
            opaque = ((i + j) & 1) == 0
            off = (y * width + x) * 4
            if opaque:
                out[off:off + 4] = b"\xff\xff\xff\xff"
            else:
                out[off:off + 4] = b"\x00\x00\x00\x00"
    return bytes(out)


# ── Opacity sampler ──────────────────────────────────────────────────────────


class OpacitySampler:
    """Opacity lookup over a decoded PNG, matching the renderer's texture
    sampling exactly: wrap-repeat addressing, **no V flip** (Vulkan/Slang
    ``Texture2D.Sample(uv)`` maps ``v = 0`` to image row 0 — the top row — and
    Aether's OBJ importer stores ``vt`` verbatim, so the bake must use the same
    convention or the baked cutout is mirrored against the shaded one). The
    opacity value is the alpha channel when present, else the luminance
    (Rec.601) of the RGB / gray texel, normalised to [0, 1]."""

    def __init__(self, width: int, height: int, channels: int, pixels: bytes):
        self.w = width
        self.h = height
        self.chans = channels
        self.px = pixels

    @classmethod
    def from_png(cls, path: Path) -> "OpacitySampler":
        w, h, ch, px = decode_png(path)
        return cls(w, h, ch, px)

    #: Texel budget for one microtriangle footprint. A UV triangle spanning more
    #: texels than this is degenerate for cutout purposes (an entire texture atlas
    #: crammed into one microtriangle); it is reported as fully ambiguous rather
    #: than scanned, which is the conservative answer in both directions.
    FOOTPRINT_TEXEL_CAP = 4096

    def _texel_opacity(self, x: int, y: int) -> float:
        i = (y % self.h) * self.w * self.chans + (x % self.w) * self.chans
        c = self.px[i:i + self.chans]
        if self.chans >= 4:
            return c[3] / 255.0
        if self.chans == 2:
            return c[1] / 255.0
        if self.chans == 3:
            return (0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]) / 255.0
        return c[0] / 255.0

    def opacity(self, u: float, v: float) -> float:
        """Nearest-texel opacity at ``(u, v)`` (wrap-repeat, no V flip)."""
        u -= math.floor(u)
        v -= math.floor(v)
        x = int(u * self.w) % self.w
        y = int(v * self.h) % self.h
        return self._texel_opacity(x, y)

    def opacity_range(
        self,
        uv0: tuple[float, float],
        uv1: tuple[float, float],
        uv2: tuple[float, float],
    ) -> tuple[float, float]:
        """``(min, max)`` opacity a renderer can sample *anywhere* inside the UV
        triangle ``(uv0, uv1, uv2)``.

        The renderer samples with ``VK_FILTER_LINEAR`` + ``REPEAT`` (see
        Harmonia ``Texture::create``), so a sample at texel-space position
        ``p`` reads the four texels around ``p - 0.5``. The scanned texel window
        is therefore the triangle's texel-space bounding box grown by the
        bilinear tap footprint. Using the bounding box rather than exact
        triangle coverage widens the window, which only ever makes the result
        *more* ambiguous — never less — so a microtriangle is declared uniform
        only when it provably is. That one-sided error is what makes the baked
        micromap a pure accelerator: it can never remove a hit the shaded
        cutout would have kept, nor keep one it would have removed."""
        xs = [uv0[0] * self.w, uv1[0] * self.w, uv2[0] * self.w]
        ys = [uv0[1] * self.h, uv1[1] * self.h, uv2[1] * self.h]
        if not all(math.isfinite(v) for v in xs + ys):
            return (0.0, 1.0) # degenerate UVs: fully ambiguous
        x0 = math.floor(min(xs) - 0.5)
        x1 = math.floor(max(xs) - 0.5) + 1
        y0 = math.floor(min(ys) - 0.5)
        y1 = math.floor(max(ys) - 0.5) + 1
        if (x1 - x0 + 1) * (y1 - y0 + 1) > self.FOOTPRINT_TEXEL_CAP:
            return (0.0, 1.0)
        lo = 1.0
        hi = 0.0
        for y in range(y0, y1 + 1):
            row = (y % self.h) * self.w * self.chans
            for x in range(x0, x1 + 1):
                i = row + (x % self.w) * self.chans
                c = self.px[i:i + self.chans]
                if self.chans >= 4:
                    a = c[3] / 255.0
                elif self.chans == 2:
                    a = c[1] / 255.0
                elif self.chans == 3:
                    a = (0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]) / 255.0
                else:
                    a = c[0] / 255.0
                if a < lo:
                    lo = a
                if a > hi:
                    hi = a
        return (lo, hi)


# ── Wavefront OBJ reader (positions + UVs + groups + triangulated faces) ─────


@dataclass
class ObjTriangle:
    """One triangulated OBJ face: three ``(position_index, uv_index)`` pairs,
    1-based as authored (0 → "no index"). Only UVs are consumed by the bake;
    positions are kept for diagnostics / validation."""

    verts: tuple[tuple[int, int], tuple[int, int], tuple[int, int]]


@dataclass
class ObjMesh:
    """Parsed OBJ: indexed positions / UVs and per-group triangle lists. Faces
    are attributed to the current ``g``/``o`` group; faces before any group
    declaration land under the object name (or ``""`` if absent)."""

    positions: list[tuple[float, float, float]] = field(default_factory=list)
    uvs: list[tuple[float, float]] = field(default_factory=list)
    groups: dict[str, list[ObjTriangle]] = field(default_factory=dict)

    def group_order(self) -> list[str]:
        """Groups in first-seen order (stable for deterministic output)."""
        return list(self.groups.keys())


def _warn(path: Path, lineno: int, message: str) -> None:
    sys.stderr.write("warning: %s:%d: %s\n" % (path, lineno, message))


def parse_obj(path: Path) -> ObjMesh:
    """Stream-parse a Wavefront OBJ into positions, UVs and per-group
    triangles. Only ``v`` / ``vt`` / ``g`` / ``o`` / ``f`` are consumed; ``vn``
    and free-form data are ignored. Faces are triangulated by fan (a/b/c, then
    a/c/d, …) so n-gon OBJs are handled. UV indices are resolved to the
    ``uv_index`` (1-based; 0 = absent). Negative (relative) indices are
    resolved against the current position/UV count."""
    mesh = ObjMesh()
    current_group = ""  # groups are created lazily on their first face

    with open(path, "rb") as f:
        for lineno, line in enumerate(f, start=1):
            head = line.lstrip()
            if head.startswith(b"v ") or head.startswith(b"v\t"):
                parts = head.split()
                if len(parts) < 4:
                    _warn(path, lineno, "ignoring 'v' with < 3 coordinates")
                    continue
                try:
                    mesh.positions.append((float(parts[1]), float(parts[2]), float(parts[3])))
                except ValueError:
                    _warn(path, lineno, "ignoring non-numeric 'v'")
            elif head.startswith(b"vt ") or head.startswith(b"vt\t"):
                parts = head.split()
                if len(parts) < 3:
                    continue
                try:
                    mesh.uvs.append((float(parts[1]), float(parts[2])))
                except ValueError:
                    _warn(path, lineno, "ignoring non-numeric 'vt'")
            elif head.startswith(b"g ") or head.startswith(b"g\t"):
                current_group = head.split(None, 1)[1].decode("utf-8", "replace").strip()
            elif head.startswith(b"o ") or head.startswith(b"o\t"):
                # 'o' only names the implicit group if no 'g' has been seen yet;
                # the list is still created lazily on the first face.
                name = head.split(None, 1)[1].decode("utf-8", "replace").strip()
                if current_group == "":
                    current_group = name
            elif head.startswith(b"f ") or head.startswith(b"f\t"):
                verts = _parse_face(head, len(mesh.positions), len(mesh.uvs))
                if verts is None or len(verts) < 3:
                    _warn(path, lineno, "ignoring face with < 3 vertices")
                    continue
                # Fan triangulation: (v0, v1, v2), (v0, v2, v3), ...
                tris = mesh.groups.setdefault(current_group, [])
                for k in range(1, len(verts) - 1):
                    tris.append(ObjTriangle(verts=(verts[0], verts[k], verts[k + 1])))
    return mesh


def _parse_face(
    head: bytes, n_pos: int, n_uv: int
) -> list[tuple[int, int]] | None:
    """Parse an ``f`` line into a list of ``(position_idx, uv_idx)`` (1-based,
    0 = absent). Handles ``v``, ``v/vt``, ``v//vn``, ``v/vt/vn`` and negative
    (relative) indices."""
    parts = head.split()[1:]
    out: list[tuple[int, int]] = []
    for tok in parts:
        if tok.endswith(b"\r"):
            tok = tok[:-1]
        if not tok:
            continue
        comps = tok.split(b"/")
        p_raw = comps[0]
        try:
            p = int(p_raw)
        except ValueError:
            return None
        if p < 0:
            p = n_pos + p + 1
        uv = 0
        if len(comps) >= 2 and comps[1] != b"":
            try:
                u = int(comps[1])
            except ValueError:
                u = 0
            if u < 0:
                u = n_uv + u + 1
            uv = u
        out.append((p, uv))
    return out if len(out) >= 3 else None


# ── Space-filling-curve index (Vulkan/glTF reference, verbatim) ──────────────


def barycentrics_to_sfc_index(u: float, v: float, level: int) -> int:
    """Map a barycentric point ``(u, v)`` (with ``w = 1 - u - v``) inside a base
    triangle to its microtriangle's space-filling-curve index at the given
    subdivision ``level``. Reproduced verbatim from the ``VK_EXT_opacity_micromap``
    / glTF ``EXT_mesh_opacity_micromap`` reference (Appendix A); the on-device
    RT traversal uses the same mapping, so bits packed in this order match."""
    u = min(max(u, 0.0), 1.0)
    v = min(max(v, 0.0), 1.0)
    fu = u * (1 << level)
    fv = v * (1 << level)
    iu = int(fu)
    iv = int(fv)
    uf = fu - iu
    vf = fv - iv
    if iu >= (1 << level):
        iu = (1 << level) - 1
    if iv >= (1 << level):
        iv = (1 << level) - 1
    iuv = iu + iv
    if iuv >= (1 << level):
        iu -= iuv - (1 << level) + 1
    iw = (~(iu + iv)) & U32
    if uf + vf >= 1.0 and iuv < (1 << level) - 1:
        iw = (iw - 1) & U32
    b0 = (~(iu ^ iw)) & U32
    b0 &= (1 << level) - 1
    t = (iu ^ iv) & b0
    f = t
    f ^= f >> 1
    f ^= f >> 2
    f ^= f >> 4
    f ^= f >> 8
    b1 = (((f ^ iu) & (~b0 & U32)) | t) & U32
    # Interleave bits of b0 / b1 into the low 2*level bits.
    b0 = (b0 | (b0 << 8)) & 0x00FF00FF
    b0 = (b0 | (b0 << 4)) & 0x0F0F0F0F
    b0 = (b0 | (b0 << 2)) & 0x33333333
    b0 = (b0 | (b0 << 1)) & 0x55555555
    b1 = (b1 | (b1 << 8)) & 0x00FF00FF
    b1 = (b1 | (b1 << 4)) & 0x0F0F0F0F
    b1 = (b1 | (b1 << 2)) & 0x33333333
    b1 = (b1 | (b1 << 1)) & 0x55555555
    return (b0 | (b1 << 1)) & U32


def microtriangle_corners(level: int) -> Iterator[
    tuple[tuple[float, float, float], tuple[float, float, float], tuple[float, float, float]]
]:
    """Yield the three barycentric corners ``(w0, w1, w2)`` of each of the
    ``4**level`` microtriangles, obtained by recursive midpoint subdivision of
    the unit simplex (corners v0=(1,0,0), v1=(0,1,0), v2=(0,0,1)). The corners
    span the microtriangle exactly, so its UV footprint is the triangle they map
    to; the centroid (their mean) lies strictly inside, so feeding it to
    ``barycentrics_to_sfc_index`` yields every index exactly once."""

    def _sub(a, b, c, lvl):
        if lvl <= 0:
            yield (a, b, c)
            return
        mab = ((a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0, (a[2] + b[2]) / 2.0)
        mbc = ((b[0] + c[0]) / 2.0, (b[1] + c[1]) / 2.0, (b[2] + c[2]) / 2.0)
        mca = ((c[0] + a[0]) / 2.0, (c[1] + a[1]) / 2.0, (c[2] + a[2]) / 2.0)
        yield from _sub(a, mab, mca, lvl - 1)
        yield from _sub(mab, b, mbc, lvl - 1)
        yield from _sub(mca, mbc, c, lvl - 1)
        yield from _sub(mab, mbc, mca, lvl - 1)

    yield from _sub((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0), level)


def microtriangle_centroids(level: int) -> Iterator[tuple[float, float, float]]:
    """Centroid of each microtriangle, in the same order as
    ``microtriangle_corners``."""
    for a, b, c in microtriangle_corners(level):
        yield (
            (a[0] + b[0] + c[0]) / 3.0,
            (a[1] + b[1] + c[1]) / 3.0,
            (a[2] + b[2] + c[2]) / 3.0,
        )


# ── Per-triangle bake ────────────────────────────────────────────────────────


@dataclass
class TriangleRecord:
    """Baked outcome for one base triangle. Either a special index (``special``
    != 0, no data consumed) or a full micromap record (``level`` / ``format`` /
    packed-state ``hex``)."""


def _state_from_range(lo: float, hi: float, fmt: int) -> int:
    """Classify one microtriangle from the ``(min, max)`` opacity a renderer can
    sample inside it (see ``OpacitySampler.opacity_range``).

    OpenPBR's α is a *presence weight*, not a threshold: the renderers resolve it
    stochastically, so a microtriangle may only be decided by traversal where α is
    constant over its whole footprint.

    * α ≡ 0 everywhere → ``STATE_TRANSPARENT``: the shaded gate would always have
      passed the ray through, so skipping it in traversal changes nothing;
    * α ≡ 1 everywhere → ``STATE_OPAQUE``: the gate would always have kept it;
    * anything else (including a partially-transparent α such as 0.4, and every
      microtriangle straddling a cut-out edge) → ``STATE_UNKNOWN_OPAQUE``, which
      hands the decision back to the shader — the identical per-hit
      ``geometry_opacity * map_opacity`` test the unaccelerated mesh runs.

    The two-state format cannot express "ask the shader": its OPAQUE would commit
    a microtriangle the gate may still pass through. A two-state bake is therefore
    only exact where every microtriangle is uniform, and falls back to TRANSPARENT
    (which the gate can still override on the surviving geometry) otherwise.
    Four-state is the default for this reason."""
    if hi <= 0.0:
        return STATE_TRANSPARENT
    if lo >= 1.0:
        return STATE_OPAQUE
    return STATE_TRANSPARENT if fmt == FORMAT_TWO_STATE else STATE_UNKNOWN_OPAQUE


def _collapse_special(states: list[int]) -> int:
    """If every microtriangle state is identical, return the matching special
    index (no micromap record needed); else 0 (a full record is required)."""
    first = states[0]
    for s in states[1:]:
        if s != first:
            return 0
    return {
        STATE_TRANSPARENT: SPECIAL_TRANSPARENT,
        STATE_OPAQUE: SPECIAL_OPAQUE,
        STATE_UNKNOWN_TRANSPARENT: SPECIAL_UNK_TRANSPARENT,
        STATE_UNKNOWN_OPAQUE: SPECIAL_UNK_OPAQUE,
    }[first]



def _pack_hex(states: list[int], fmt: int) -> str:
    """Pack microtriangle states (SFC order, index 0 first) into a hex string —
    the dump of the Vulkan ``data`` bytes, LSB-first within each byte. Two-state
    uses 1 bit/microtriangle; four-state expands each state to 2 bits (low then
    high) at ``bits 2i, 2i+1``."""
    if fmt == FORMAT_TWO_STATE:
        bits = states
    else:
        bits = []
        for s in states:
            bits.append(s & 1)
            bits.append((s >> 1) & 1)
    nbytes = (len(bits) + 7) // 8
    ba = bytearray(nbytes)
    for i, bit in enumerate(bits):
        if bit:
            ba[i >> 3] |= 1 << (i & 7)
    return ba.hex().upper()


@dataclass
class BakedTriangle:
    """Result of baking one base triangle: either ``special`` (!= 0) or a
    ``(level, fmt, hex)`` record."""

    special: int = 0
    level: int = 0
    fmt: int = 0
    hex: str = ""


def bake_triangle(
    uv0: tuple[float, float],
    uv1: tuple[float, float],
    uv2: tuple[float, float],
    sampler: OpacitySampler,
    level: int,
    fmt: int,
    scalar: float,
) -> BakedTriangle:
    """Bake one base triangle: subdivide into ``4**level`` microtriangles,
    conservatively range-sample the opacity texture over each microtriangle's UV
    footprint, classify, and pack in SFC order. Uniform triangles collapse to a
    special index."""
    n = 4 ** level
    states = [STATE_OPAQUE] * n
    for (a, b, c) in microtriangle_corners(level):
        muv = [
            (
                w[0] * uv0[0] + w[1] * uv1[0] + w[2] * uv2[0],
                w[0] * uv0[1] + w[1] * uv1[1] + w[2] * uv2[1],
            )
            for w in (a, b, c)
        ]
        lo, hi = sampler.opacity_range(muv[0], muv[1], muv[2])
        centroid = ((a[1] + b[1] + c[1]) / 3.0, (a[2] + b[2] + c[2]) / 3.0)
        idx = barycentrics_to_sfc_index(centroid[0], centroid[1], level)
        states[idx] = _state_from_range(lo * scalar, hi * scalar, fmt)
    special = _collapse_special(states)
    if special != 0:
        return BakedTriangle(special=special)
    return BakedTriangle(level=level, fmt=fmt, hex=_pack_hex(states, fmt))


# ── Histogram ────────────────────────────────────────────────────────────────


@dataclass
class GroupBake:
    """All baked triangles for one OBJ group + its usage histogram. The
    histogram counts only full ``T`` records (special-index triangles consume no
    micromap data), matching ``VkMicromapUsageEXT``."""

    name: str
    source_material: str
    triangle_count: int  # total base triangles (T + S) == OBJ group face count
    records: list[BakedTriangle] = field(default_factory=list)

    def usage(self) -> list[tuple[int, int, int]]:
        """``[(count, subdivision_level, format), ...]`` sorted for stable
        output."""
        counts: dict[tuple[int, int], int] = {}
        for rec in self.records:
            if rec.special == 0:
                counts[(rec.level, rec.fmt)] = counts.get((rec.level, rec.fmt), 0) + 1
        return [(c, lvl, fmt) for (lvl, fmt), c in sorted(counts.items())]


# ── Material / scene discovery ───────────────────────────────────────────────


@dataclass
class OpacityInput:
    """Resolved opacity input for one group's material."""

    material: str
    texture_path: Path
    scalar: float
    level: int
    fmt: int


def _load_material_library(path: Path) -> dict[str, dict]:
    """Parse a material-library TOML into ``{name: table}`` (skipping the
    file-level ``model`` / ``colorspace`` scalar keys)."""
    with open(path, "rb") as f:
        root = tomllib.load(f)
    return {
        key: val
        for key, val in root.items()
        if isinstance(val, dict) and key not in ("render", "camera", "tonemap")
    }


def _material_opacity(mat: dict, lib_dir: Path) -> tuple[str | None, float]:
    """Return ``(texture_path_or_None, geometry_opacity)`` for a material table.
    A material with no ``map_opacity`` returns ``None`` — the filter signal."""
    tex = mat.get("map_opacity")
    if isinstance(tex, str) and tex:
        tex = str((lib_dir / tex).resolve())
    else:
        tex = None
    return tex, float(mat.get("geometry_opacity", 1.0))


@dataclass
class SceneTarget:
    """Resolved scene-driven bake target: the OBJ path, the mesh name, and the
    group -> opacity-input map (only groups whose material declares
    ``map_opacity`` — the hero-material filter)."""

    obj_path: Path
    mesh_name: str
    group_inputs: dict[str, OpacityInput]
    scene_path: Path
    lib_path: Path


def resolve_scene(
    scene_path: Path,
    mesh_name: str,
    groups_filter: list[str] | None,
    level: int,
    fmt: int,
) -> SceneTarget:
    """Read the scene + its first material library, find the named mesh's OBJ
    path and the instance group->material map, then keep only groups whose
    material declares ``map_opacity`` (optionally further restricted by
    ``groups_filter``). This is the hero-material filter: neutrals without an
    opacity texture are skipped, so only the hero zones are baked."""
    with open(scene_path, "rb") as f:
        scene = tomllib.load(f)
    libs = scene.get("material_libraries", [])
    if not libs:
        raise ValueError("%s: no material_libraries declared" % scene_path)
    lib_path = (scene_path.parent / libs[0]).resolve()
    materials = _load_material_library(lib_path)

    # Find the mesh's OBJ path.
    obj_rel = None
    for m in scene.get("mesh", []):
        if m.get("name") == mesh_name:
            obj_rel = m.get("path")
            break
    if obj_rel is None:
        raise ValueError(
            "%s: no [[mesh]] with name = \"%s\"" % (scene_path, mesh_name)
        )
    obj_path = (scene_path.parent / obj_rel).resolve()

    # Find an instance of this mesh and its group->material map.
    group_to_mat: dict[str, str] = {}
    for inst in scene.get("instance", []):
        if inst.get("mesh") != mesh_name:
            continue
        gm = inst.get("materials", {})
        if isinstance(gm, dict):
            group_to_mat.update({str(k): str(v) for k, v in gm.items()})
        if inst.get("material"):
            # whole-instance material applies to groups without an explicit entry
            inst_mat = str(inst["material"])
            for g in group_to_mat:
                group_to_mat.setdefault(g, inst_mat)
        break
    if not group_to_mat:
        raise ValueError(
            "%s: instance of mesh \"%s\" has no materials map" % (scene_path, mesh_name)
        )

    group_inputs: dict[str, OpacityInput] = {}
    for grp, mat_name in group_to_mat.items():
        if groups_filter is not None and grp not in groups_filter:
            continue
        mat = materials.get(mat_name)
        if mat is None:
            sys.stderr.write(
                "warning: %s: material \"%s\" (group \"%s\") not found — skipping\n"
                % (scene_path.name, mat_name, grp)
            )
            continue
        tex, scalar = _material_opacity(mat, lib_path.parent)
        if tex is None:
            continue  # filter: no map_opacity -> not a hero/opacity group
        group_inputs[grp] = OpacityInput(
            material=mat_name,
            texture_path=Path(tex),
            scalar=scalar,
            level=level,
            fmt=fmt,
        )
    if not group_inputs:
        raise ValueError(
            "%s: no group of mesh \"%s\" has a material with map_opacity"
            % (scene_path.name, mesh_name)
        )
    return SceneTarget(
        obj_path=obj_path,
        mesh_name=mesh_name,
        group_inputs=group_inputs,
        scene_path=scene_path,
        lib_path=lib_path,
    )


# ── Bake driver ──────────────────────────────────────────────────────────────


def bake_group(
    obj: ObjMesh,
    group: str,
    inp: OpacityInput,
) -> GroupBake:
    """Bake one OBJ group's triangles against the resolved opacity input."""
    tris = obj.groups.get(group)
    if tris is None:
        raise ValueError("OBJ has no group \"%s\"" % group)
    sampler = OpacitySampler.from_png(inp.texture_path)
    records: list[BakedTriangle] = []
    for tri in tris:
        (_p0, u0), (_p1, u1), (_p2, u2) = tri.verts
        uv0 = obj.uvs[u0 - 1] if 0 < u0 <= len(obj.uvs) else (0.0, 0.0)
        uv1 = obj.uvs[u1 - 1] if 0 < u1 <= len(obj.uvs) else (0.0, 0.0)
        uv2 = obj.uvs[u2 - 1] if 0 < u2 <= len(obj.uvs) else (0.0, 0.0)
        if u0 == 0 or u1 == 0 or u2 == 0:
            # No UVs -> the shader samples map_opacity at (0,0) for the whole
            # triangle; hand the decision to the shader rather than guess.
            records.append(BakedTriangle(special=SPECIAL_UNK_OPAQUE))
            continue
        records.append(
            bake_triangle(uv0, uv1, uv2, sampler, inp.level, inp.fmt, inp.scalar)
        )
    return GroupBake(
        name=group, source_material=inp.material, triangle_count=len(tris), records=records
    )


# ── .omm text sidecar emission ───────────────────────────────────────────────


def render_omm(
    obj_path: Path,
    groups: list[GroupBake],
    inputs: dict[str, OpacityInput],
    texture_path: Path,
) -> str:
    """Render the OBJ-style ``.omm`` text sidecar. One ``G <group>`` section per
    baked group; within, ``T`` / ``S`` lines are the group's base triangles in
    OBJ face order (positional — loader validates count == OBJ group faces)."""
    le = "\n"
    lines: list[str] = [
        "# %s opacity micromap text source (omm-text/v1)" % obj_path.name,
        "# Packs to VkMicromapTriangleKHR[] + packed-state data (LSB-first, SFC order).",
        "# Each 'T <level> <format> <hex>' is one base triangle's record;",
        "# 'S <special>' is a fully-uniform triangle",
        "#   (-1 transparent / -2 opaque / -3 unknown-transparent / -4 unknown-opaque;",
        "#    the 'unknown' pair means: traversal asks the shader per hit).",
        "# Source texture: %s" % texture_path.name,
        "",
    ]
    for gb in groups:
        inp = inputs[gb.name]
        lines.append(
            "G %s%s# %d triangles, material=%s, level=%d, format=%d, geometry_opacity=%s"
            % (gb.name, le, gb.triangle_count, gb.source_material, inp.level, inp.fmt, fmt(inp.scalar))
        )
        for rec in gb.records:
            if rec.special != 0:
                lines.append("S %d" % rec.special)
            else:
                lines.append("T %d %d %s" % (rec.level, rec.fmt, rec.hex))
        lines.append("")
    return le.join(lines).rstrip() + le


# ── .micromap.toml descriptor emission ───────────────────────────────────────


def _toml_inline_usage(usage: list[tuple[int, int, int]]) -> str:
    """Render a usage histogram as a TOML inline array of inline tables."""
    if not usage:
        return "[]"
    return (
        "["
        + ", ".join(
            "{ count = %d, subdivision_level = %d, format = %d }" % (c, lvl, fmt)
            for (c, lvl, fmt) in usage
        )
        + "]"
    )


def render_micromap_toml(
    obj_path: Path,
    scene_path: Path | None,
    lib_path: Path | None,
    texture_path: Path,
    groups: list[GroupBake],
    inputs: dict[str, OpacityInput],
    omm_filename: str,
    default_level: int,
    default_fmt: int,
) -> str:
    """Render the ``.micromap.toml`` descriptor: parameters, the ``[data]``
    sidecar reference, recorded sources (mesh + material + texture) for
    ``--check``, and one ``[[group]]`` per baked group with its usage
    histogram."""
    base = lambda p: p.name  # relative filenames keep the asset portable
    lines: list[str] = [
        "# opacity micromap descriptor (Aether omm-text/v1).",
        "# The baked OMM = (mesh UVs) x (material map_opacity); both sources are",
        "# recorded below so 'omm_bake.py --check' can re-bake and detect drift.",
        'format = "omm-text/v1"',
        'source_obj = "%s"' % base(obj_path),
    ]
    if scene_path is not None:
        lines.append('source_scene = "%s"' % base(scene_path))
    if lib_path is not None:
        lines.append('source_material_library = "%s"' % base(lib_path))
    lines.append('source_texture = "%s"' % base(texture_path))
    lines.extend(
        [
            "default_subdivision_level = %d" % default_level,
            "default_format = %d" % default_fmt,
            "",
            "[data]",
            'file = "%s"' % omm_filename,
            "",
        ]
    )
    for gb in groups:
        inp = inputs[gb.name]
        usage = gb.usage()
        n_records = sum(c for (c, _l, _f) in usage)
        n_special = gb.triangle_count - n_records
        lines.append("[[group]]")
        lines.append('name = "%s"' % gb.name)
        lines.append('source_material = "%s"' % gb.source_material)
        lines.append("triangle_count = %d  # base triangles (T + S)" % gb.triangle_count)
        lines.append(
            "record_count = %d  # T records (special = %d)"
            % (n_records, n_special)
        )
        lines.append("usage = %s" % _toml_inline_usage(usage))
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


# ── Scene .toml in-place writer (inject opacity_micromaps into [[mesh]]) ──────


def _split_le(line: str) -> tuple[str, str]:
    if line.endswith("\r\n"):
        return line[:-2], "\r\n"
    if line.endswith("\n"):
        return line[:-1], "\n"
    if line.endswith("\r"):
        return line[:-1], "\r"
    return line, ""


import re  # noqa: E402

_RE_ARRAY_HEADER = re.compile(r"^\s*\[\[([A-Za-z0-9_]+)\]\]\s*$")
_RE_TABLE_HEADER = re.compile(r"^\s*\[([A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)*)\]\s*$")
_RE_NAME_VALUE = re.compile(r"""^\s*name\s*=\s*["']([^"']+)['"]\s*$""")
_RE_OPACITY_MICROMAPS = re.compile(r"^\s*opacity_micromaps\s*=.*$")


def _find_mesh_by_name(lines: list[str], mesh_name: str) -> tuple[int, int] | None:
    """Locate the ``[[mesh]]`` entry whose ``name`` matches. Returns
    ``(start, end)`` spanning the whole entry (header + body + sub-tables)."""
    n = len(lines)
    i = 0
    while i < n:
        core, _ = _split_le(lines[i])
        m = _RE_ARRAY_HEADER.match(core)
        if not m or m.group(1) != "mesh":
            i += 1
            continue
        end = _entry_end(lines, i + 1, "mesh", n)
        for k in range(i + 1, end):
            corek, _ = _split_le(lines[k])
            nm = _RE_NAME_VALUE.match(corek)
            if nm and nm.group(1) == mesh_name:
                return i, end
        i = end
    return None


def _entry_end(lines: list[str], j: int, array_key: str, n: int) -> int:
    while j < n:
        core, _ = _split_le(lines[j])
        if _RE_ARRAY_HEADER.match(core):
            return j
        m = _RE_TABLE_HEADER.match(core)
        if m and m.group(1).split(".")[0] != array_key:
            return j
        j += 1
    return n


def _last_kv_index(lines: list[str]) -> int:
    last = -1
    for idx, line in enumerate(lines):
        core, _ = _split_le(line)
        stripped = core.strip()
        if not stripped or stripped.startswith("#") or stripped.startswith("["):
            continue
        if "=" in core:
            last = idx
    return last


def _atomic_write_text(path: Path, text: str) -> None:
    directory = path.parent if str(path.parent) else Path(".")
    fd, tmp = tempfile.mkstemp(suffix=".tmp", dir=str(directory))
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise


def inject_opacity_micromaps(
    scene_path: Path, mesh_name: str, group_to_file: dict[str, str]
) -> None:
    """Inject / replace ``opacity_micromaps = { ... }`` in the scene's
    ``[[mesh]] name = mesh_name`` entry. Atomically rewritten."""
    text = scene_path.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)
    found = _find_mesh_by_name(lines, mesh_name)
    if found is None:
        raise ValueError(
            "%s: no [[mesh]] with name = \"%s\"" % (scene_path, mesh_name)
        )
    start, end = found
    body = lines[start:end]
    _, le = _split_le(body[0]) if body else ("\n", "\n")
    if not le:
        le = "\n"

    inline = (
        "opacity_micromaps = { "
        + ", ".join('"%s" = "%s"' % (g, f) for g, f in group_to_file.items())
        + " }"
    )

    # Replace an existing opacity_micromaps line in-place, else insert after the
    # last key/value line of the entry body.
    replaced = False
    new_body: list[str] = []
    for line in body:
        core, _le = _split_le(line)
        if _RE_OPACITY_MICROMAPS.match(core):
            new_body.append(inline + le)
            replaced = True
        else:
            new_body.append(line)
    if not replaced:
        insert_at = _last_kv_index(new_body) + 1
        if (
            insert_at > 0
            and _split_le(new_body[insert_at - 1])[0].strip() != ""
        ):
            new_body.insert(insert_at, le)
            insert_at += 1
        new_body.insert(insert_at, inline + le)

    new_lines = lines[:start] + new_body + lines[end:]
    _atomic_write_text(scene_path, "".join(new_lines))


# ── .omm re-parse (for --check / round-trip) ─────────────────────────────────


@dataclass
class ParsedOmm:
    """Re-parsed ``.omm`` sidecar: per-group ordered list of records (each
    either ``("S", special)`` or ``("T", level, fmt, hex)``)."""

    groups: dict[str, list[tuple]] = field(default_factory=dict)


def parse_omm(path: Path) -> ParsedOmm:
    """Parse an ``.omm`` text sidecar back into structured records. Validates
    hex evenness and the 2-state/four-state byte-length relationship."""
    omm = ParsedOmm()
    current = None
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("G "):
            current = line[2:].strip()
            omm.groups[current] = []
            continue
        if current is None:
            raise ValueError("%s:%d: record before any 'G' group" % (path, lineno))
        if line.startswith("S "):
            try:
                special = int(line[2:].strip())
            except ValueError:
                raise ValueError("%s:%d: bad special index" % (path, lineno))
            omm.groups[current].append(("S", special))
        elif line.startswith("T "):
            parts = line[2:].split()
            if len(parts) != 3:
                raise ValueError("%s:%d: 'T' needs <level> <format> <hex>" % (path, lineno))
            try:
                level = int(parts[0])
                fmt = int(parts[1])
            except ValueError:
                raise ValueError("%s:%d: bad level/format" % (path, lineno))
            hexbits = parts[2]
            _validate_hex(hexbits, level, fmt, path, lineno)
            omm.groups[current].append(("T", level, fmt, hexbits))
        else:
            raise ValueError("%s:%d: unknown record '%s'" % (path, lineno, line[:12]))
    return omm


def _validate_hex(hexbits: str, level: int, fmt: int, path: Path, lineno: int) -> None:
    if fmt not in (FORMAT_TWO_STATE, FORMAT_FOUR_STATE):
        raise ValueError("%s:%d: bad format %d" % (path, lineno, fmt))
    if level < 0:
        raise ValueError("%s:%d: bad level %d" % (path, lineno, level))
    bits_per = 1 if fmt == FORMAT_TWO_STATE else 2
    expected_bits = (4 ** level) * bits_per
    expected_hex = 2 * ((expected_bits + 7) // 8)
    if len(hexbits) != expected_hex:
        raise ValueError(
            "%s:%d: hex length %d != expected %d for level=%d format=%d"
            % (path, lineno, len(hexbits), expected_hex, level, fmt)
        )
    try:
        int(hexbits, 16)
    except ValueError:
        raise ValueError("%s:%d: non-hex characters" % (path, lineno))


# ── CLI ──────────────────────────────────────────────────────────────────────


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=(
            "Bake a VK_EXT_opacity_micromap from an OBJ + a material's "
            "map_opacity texture (Aether .omm + .micromap.toml)."
        )
    )
    p.add_argument("obj", nargs="?", help="Input .obj file (explicit mode)")
    p.add_argument("--scene", metavar="TOML", help="Scene .toml (scene-driven discovery mode)")
    p.add_argument("--mesh", metavar="NAME", help="Mesh name within --scene to bake")
    p.add_argument(
        "--material-lib", metavar="TOML", help="Material library .toml (explicit mode)"
    )
    p.add_argument("--material", metavar="NAME", help="Material name (explicit mode)")
    p.add_argument(
        "--groups",
        metavar="G1,G2",
        help="Restrict to these OBJ groups (explicit mode, or a filter on scene mode)",
    )
    p.add_argument("--level", type=int, default=2, help="Subdivision level (default 2)")
    p.add_argument(
        "--format",
        type=int,
        choices=(FORMAT_TWO_STATE, FORMAT_FOUR_STATE),
        default=FORMAT_FOUR_STATE,
        help=(
            "Opacity format: 2=4-state (default; carries the 'ask the shader' "
            "states an exact cutout needs), 1=2-state (only exact when every "
            "microtriangle is uniform)"
        ),
    )
    p.add_argument("--out-omm", metavar="PATH", help="Output .omm sidecar path")
    p.add_argument("--out-toml", metavar="PATH", help="Output .micromap.toml path")
    p.add_argument(
        "--write",
        action="store_true",
        help="Inject opacity_micromaps into the scene's [[mesh]] (scene mode)",
    )
    p.add_argument(
        "--emit-checker",
        nargs=5,
        metavar=("W", "H", "CELLSU", "CELLSV", "OUT"),
        help="Emit a checkerboard opacity PNG and exit",
    )
    p.add_argument(
        "--check", metavar="TOML", help="Re-bake from a .micromap.toml's sources and diff"
    )
    p.add_argument(
        "--inspect",
        metavar="TOML",
        help="Dump one triangle's microtriangle states (needs --group/--triangle)",
    )
    p.add_argument("--group", metavar="NAME", help="Group for --inspect")
    p.add_argument("--triangle", type=int, default=0, help="Triangle index for --inspect")
    p.add_argument("--quiet", action="store_true", help="Suppress the summary")
    return p


def _run_emit_checker(args) -> int:
    w, h, cu, cv, out = args.emit_checker
    w_i, h_i, cu_i, cv_i = int(w), int(h), int(cu), int(cv)
    pixels = make_checker_rgba(w_i, h_i, cu_i, cv_i)
    Path(out).write_bytes(encode_png_rgba(w_i, h_i, pixels))
    if not args.quiet:
        sys.stderr.write(
            "wrote %s (%dx%d, %dx%d cells)\n" % (out, w_i, h_i, cu_i, cv_i)
        )
    return 0


def _run_explicit(args) -> tuple[Path, Path | None, dict[str, OpacityInput], Path]:
    obj_path = Path(args.obj)
    lib_path = Path(args.material_lib)
    materials = _load_material_library(lib_path)
    mat = materials.get(args.material)
    if mat is None:
        raise ValueError("material \"%s\" not in %s" % (args.material, lib_path))
    tex, scalar = _material_opacity(mat, lib_path.parent)
    if tex is None:
        raise ValueError(
            "material \"%s\" has no map_opacity — nothing to bake" % args.material
        )
    groups = [g for g in (args.groups.split(",") if args.groups else []) if g]
    if not groups:
        raise ValueError("explicit mode needs --groups G1,G2")
    inp = OpacityInput(
        material=args.material,
        texture_path=Path(tex),
        scalar=scalar,
        level=args.level,
        fmt=args.format,
    )
    inputs = {g: inp for g in groups}
    return obj_path, None, inputs, Path(tex)


def _run_scene(args) -> tuple[Path, Path, dict[str, OpacityInput], Path, Path]:
    if not args.scene or not args.mesh:
        raise ValueError("--scene and --mesh are required for scene-driven mode")
    scene_path = Path(args.scene)
    groups_filter = (
        [g.strip() for g in args.groups.split(",")] if args.groups else None
    )
    target = resolve_scene(
        scene_path,
        args.mesh,
        groups_filter,
        args.level,
        args.format,
    )
    return (
        target.obj_path,
        target.scene_path,
        target.group_inputs,
        next(iter(target.group_inputs.values())).texture_path,
        target.lib_path,
    )


def _default_output_names(stem: str, out_omm: str | None, out_toml: str | None, out_dir: Path) -> tuple[Path, Path, str]:
    """Default output paths for a bake stem (scene or OBJ derived). The OMM is
    specific to the (mesh, material) pair, so the scene stem is preferred in
    scene mode — multiple scenes sharing one OBJ get distinct OMM files."""
    full = stem + "_omm"
    omm_path = Path(out_omm) if out_omm else (out_dir / (full + ".omm"))
    toml_path = Path(out_toml) if out_toml else (out_dir / (full + ".micromap.toml"))
    return omm_path, toml_path, omm_path.name


def _run_bake(args) -> int:
    if args.scene:
        obj_path, scene_path, inputs, texture_path, lib_path = _run_scene(args)
    else:
        if not args.obj:
            raise ValueError("either --scene or a positional <obj> is required")
        obj_path, scene_path, inputs, texture_path = _run_explicit(args)
        lib_path = Path(args.material_lib)

    obj = parse_obj(obj_path)
    # Bake groups in OBJ first-seen order for stable output.
    ordered = [g for g in obj.group_order() if g in inputs]
    missing = [g for g in inputs if g not in obj.groups]
    if missing:
        raise ValueError("OBJ %s has no group(s): %s" % (obj_path.name, ", ".join(missing)))

    group_bakes: list[GroupBake] = []
    for g in ordered:
        inp = inputs[g]
        if not args.quiet:
            sys.stderr.write(
                "baking %s :: %s (%d tris, level=%d, fmt=%d)...\n"
                % (obj_path.name, g, len(obj.groups[g]), inp.level, inp.fmt)
            )
        group_bakes.append(bake_group(obj, g, inp))

    out_dir = (scene_path.parent if scene_path else obj_path.parent)
    if scene_path is not None:
        scene_name = scene_path.name
        stem = scene_name[:-len(".scene.toml")] if scene_name.endswith(".scene.toml") else scene_path.stem
    else:
        stem = obj_path.stem
    omm_path, toml_path, omm_name = _default_output_names(
        stem, args.out_omm, args.out_toml, out_dir
    )
    omm_text = render_omm(obj_path, group_bakes, inputs, texture_path)
    toml_text = render_micromap_toml(
        obj_path,
        scene_path,
        lib_path if args.scene else None,
        texture_path,
        group_bakes,
        inputs,
        omm_name,
        args.level,
        args.format,
    )
    _atomic_write_text(omm_path, omm_text)
    _atomic_write_text(toml_path, toml_text)

    if args.write:
        if scene_path is None:
            raise ValueError("--write requires --scene")
        group_to_file = {gb.name: toml_path.name for gb in group_bakes}
        inject_opacity_micromaps(scene_path, args.mesh, group_to_file)
        if not args.quiet:
            sys.stderr.write("wrote opacity_micromaps into %s\n" % scene_path)

    if not args.quiet:
        for gb in group_bakes:
            usage = gb.usage()
            n_rec = sum(c for (c, _l, _f) in usage)
            sys.stderr.write(
                "  %s: %d tris -> %d records + %d special [%s]\n"
                % (
                    gb.name,
                    gb.triangle_count,
                    n_rec,
                    gb.triangle_count - n_rec,
                    ", ".join("L%dF%dx%d" % (l, f, c) for (c, l, f) in usage) or "none",
                )
            )
        sys.stderr.write("wrote %s + %s\n" % (omm_path, toml_path))
    return 0


def _run_check(args) -> int:
    """Re-bake from a .micromap.toml's recorded sources and diff against the
    stored .omm. Detects mesh x material drift (changed UVs / texture /
    threshold / level)."""
    toml_path = Path(args.check)
    with open(toml_path, "rb") as f:
        desc = tomllib.load(f)
    omm_rel = desc["data"]["file"]
    omm_path = (toml_path.parent / omm_rel).resolve()
    obj_path = (toml_path.parent / desc["source_obj"]).resolve()
    texture_path = (toml_path.parent / desc["source_texture"]).resolve()
    default_level = int(desc.get("default_subdivision_level", 2))
    default_fmt = int(desc.get("default_format", FORMAT_FOUR_STATE))

    obj = parse_obj(obj_path)
    omm = parse_omm(omm_path)
    errors: list[str] = []
    for gdesc in desc.get("group", []):
        name = gdesc["name"]
        tricount = int(gdesc["triangle_count"])
        src_mat = gdesc.get("source_material", "?")
        # Re-derive the opacity input from the recorded material (if the lib is
        # still reachable); else fall back to the descriptor defaults.
        inp = OpacityInput(
            material=src_mat,
            texture_path=texture_path,
            scalar=1.0,
            level=default_level,
            fmt=default_fmt,
        )
        lib_rel = desc.get("source_material_library")
        if lib_rel:
            lib_path = (toml_path.parent / lib_rel).resolve()
            if lib_path.exists():
                mats = _load_material_library(lib_path)
                mat = mats.get(src_mat)
                if mat is not None:
                    _tex, scalar = _material_opacity(mat, lib_path.parent)
                    inp.scalar = scalar
        if name not in obj.groups:
            errors.append("group %s: OBJ %s no longer has it" % (name, obj_path.name))
            continue
        tris = obj.groups[name]
        if len(tris) != tricount:
            errors.append(
                "group %s: triangle_count %d -> %d (UVs changed)"
                % (name, tricount, len(tris))
            )
        stored = omm.groups.get(name, [])
        if len(stored) != len(tris):
            errors.append(
                "group %s: .omm records %d != OBJ triangles %d"
                % (name, len(stored), len(tris))
            )
            continue
        sampler = OpacitySampler.from_png(texture_path)
        for i, tri in enumerate(tris):
            (_p0, u0), (_p1, u1), (_p2, u2) = tri.verts
            uv0 = obj.uvs[u0 - 1] if 0 < u0 <= len(obj.uvs) else (0.0, 0.0)
            uv1 = obj.uvs[u1 - 1] if 0 < u1 <= len(obj.uvs) else (0.0, 0.0)
            uv2 = obj.uvs[u2 - 1] if 0 < u2 <= len(obj.uvs) else (0.0, 0.0)
            rec = bake_triangle(
                uv0, uv1, uv2, sampler, inp.level, inp.fmt, inp.scalar
            )
            stored_rec = stored[i]
            if stored_rec[0] == "S":
                if rec.special != stored_rec[1]:
                    errors.append(
                        "group %s tri %d: special %d -> %d"
                        % (name, i, stored_rec[1], rec.special)
                    )
            else:
                _t, lvl, fmt, hexbits = stored_rec
                want = BakedTriangle(level=inp.level, fmt=inp.fmt, hex=rec.hex)
                if rec.special != 0 or want.hex != hexbits or want.level != lvl or want.fmt != fmt:
                    errors.append(
                        "group %s tri %d: bits drift (%s -> %s)"
                        % (name, i, hexbits, rec.hex or ("S%d" % rec.special))
                    )
    if errors:
        for e in errors[:20]:
            sys.stderr.write("drift: %s\n" % e)
        sys.stderr.write("--check: %d divergence(s)\n" % len(errors))
        return 1
    if not args.quiet:
        sys.stderr.write("--check: %s in sync with sources\n" % toml_path.name)
    return 0


def _run_inspect(args) -> int:
    toml_path = Path(args.inspect)
    with open(toml_path, "rb") as f:
        desc = tomllib.load(f)
    omm_path = (toml_path.parent / desc["data"]["file"]).resolve()
    omm = parse_omm(omm_path)
    group = args.group
    if group is None or group not in omm.groups:
        raise ValueError("group %r not in %s" % (group, omm_path))
    recs = omm.groups[group]
    idx = args.triangle
    if idx < 0 or idx >= len(recs):
        raise ValueError("triangle %d out of range (group has %d)" % (idx, len(recs)))
    rec = recs[idx]
    if rec[0] == "S":
        sys.stdout.write("group %s tri %d: special %d\n" % (group, idx, rec[1]))
        return 0
    _t, level, fmt, hexbits = rec
    bits_per = 1 if fmt == FORMAT_TWO_STATE else 2
    n = 4 ** level
    # Re-expand hex -> per-microtriangle state in SFC order.
    raw = int(hexbits, 16)
    nbytes = len(hexbits) // 2
    ba = raw.to_bytes(nbytes, "little")
    states = []
    for i in range(n):
        if fmt == FORMAT_TWO_STATE:
            states.append((ba[i >> 3] >> (i & 7)) & 1)
        else:
            states.append((ba[(2 * i) >> 3] >> ((2 * i) & 7)) & 3)
    sys.stdout.write(
        "group %s tri %d: level=%d format=%d (%d microtriangles)\n"
        % (group, idx, level, fmt, n)
    )
    names = {
        STATE_TRANSPARENT: "transparent",
        STATE_OPAQUE: "opaque",
        STATE_UNKNOWN_TRANSPARENT: "unknown-transparent (shader decides)",
        STATE_UNKNOWN_OPAQUE: "unknown-opaque (shader decides)",
    }
    sys.stdout.write("  SFC index : state\n")
    for i, s in enumerate(states):
        sys.stdout.write("  %3d : %d  %s\n" % (i, s, names.get(s, "?")))
    return 0


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        if args.emit_checker:
            return _run_emit_checker(args)
        if args.check:
            return _run_check(args)
        if args.inspect:
            return _run_inspect(args)
        return _run_bake(args)
    except ValueError as exc:
        sys.stderr.write("error: %s\n" % exc)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
