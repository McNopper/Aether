"""Rotate an equirectangular EXR by XYZ Euler degrees without changing radiance.

Direction convention: u = atan2(z,x)/(2*pi)+0.5 and v = acos(y)/pi, with v=0
at +Y. The angles describe local-to-world rotation (Rz @ Ry @ Rx); output world
rays therefore sample the source through its inverse. Requires OpenEXR only for
the CLI; the array functions need NumPy.
"""
from __future__ import annotations

import argparse
import math
from pathlib import Path

import numpy as np


def rotation_xyz_degrees(angles) -> np.ndarray:
    x, y, z = (math.radians(float(v)) for v in angles)
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]], dtype=np.float64)
    ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]], dtype=np.float64)
    rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]], dtype=np.float64)
    return rz @ ry @ rx


def latlong_direction(u, v) -> np.ndarray:
    phi = (np.asarray(u) - 0.5) * (2.0 * math.pi)
    theta = np.asarray(v) * math.pi
    sin_theta = np.sin(theta)
    return np.stack((sin_theta * np.cos(phi), np.cos(theta), sin_theta * np.sin(phi)), axis=-1)


def direction_to_latlong(direction):
    direction = np.asarray(direction)
    direction = direction / np.linalg.norm(direction, axis=-1, keepdims=True)
    u = np.arctan2(direction[..., 2], direction[..., 0]) / (2.0 * math.pi) + 0.5
    v = np.arccos(np.clip(direction[..., 1], -1.0, 1.0)) / math.pi
    return u, v


def sample_bilinear(pixels: np.ndarray, u, v) -> np.ndarray:
    height, width = pixels.shape[:2]
    x = (np.asarray(u) * width - 0.5) % width
    y = np.clip(np.asarray(v) * height - 0.5, 0.0, height - 1.0)
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    x1 = (x0 + 1) % width
    y1 = np.minimum(y0 + 1, height - 1)
    wx = (x - x0)[..., None]
    wy = (y - y0)[..., None]
    top = pixels[y0, x0] * (1.0 - wx) + pixels[y0, x1] * wx
    bottom = pixels[y1, x0] * (1.0 - wx) + pixels[y1, x1] * wx
    return top * (1.0 - wy) + bottom * wy


def rotate_latlong(pixels: np.ndarray, angles_xyz_degrees) -> np.ndarray:
    """Return a same-size float32 map with local-to-world XYZ Euler rotation."""
    source = np.asarray(pixels, dtype=np.float32)
    if source.ndim != 3:
        raise ValueError("pixels must have shape (height,width,channels)")
    height, width = source.shape[:2]
    u = (np.arange(width, dtype=np.float64) + 0.5) / width
    v = (np.arange(height, dtype=np.float64) + 0.5) / height
    world = latlong_direction(*np.meshgrid(u, v))
    inverse = rotation_xyz_degrees(angles_xyz_degrees).T
    local = world @ inverse.T
    source_u, source_v = direction_to_latlong(local)
    return sample_bilinear(source, source_u, source_v).astype(np.float32)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--rotation", nargs=3, type=float, required=True, metavar=("X", "Y", "Z"))
    args = parser.parse_args()

    import OpenEXR

    with OpenEXR.File(str(args.input)) as source:
        channels = source.channels()
        key = "RGBA" if "RGBA" in channels else "RGB"
        pixels = channels[key].pixels
        header = source.header()
    rotated = rotate_latlong(pixels, args.rotation)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with OpenEXR.File(header, {key: OpenEXR.Channel(rotated)}) as output:
        output.write(str(args.output))
    print(f"ROTATED_LATLONG {args.output} {rotated.shape[1]}x{rotated.shape[0]} rotation={args.rotation}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
