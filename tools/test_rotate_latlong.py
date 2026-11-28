"""CPU tests for tools/rotate_latlong.py; run with Python, no EXR dependency."""
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rotate_latlong import (
    direction_to_latlong,
    latlong_direction,
    rotate_latlong,
    rotation_xyz_degrees,
)


def main() -> int:
    x = latlong_direction(0.5, 0.5)
    assert np.allclose(x, [1, 0, 0], atol=1e-12)
    u, v = direction_to_latlong(x)
    assert np.allclose([u, v], [0.5, 0.5], atol=1e-12)
    rotated = rotation_xyz_degrees([0, 90, 0]) @ np.array([1.0, 0.0, 0.0])
    assert np.allclose(rotated, [0, 0, -1], atol=1e-12)

    pixels = np.arange(8 * 16 * 3, dtype=np.float32).reshape(8, 16, 3) / 1000
    identity = rotate_latlong(pixels, [0, 0, 0])
    assert identity.shape == pixels.shape
    assert np.max(np.abs(identity - pixels)) < 2e-5

    # A local-to-world +90-degree Y turn maps world -Z back to source +X.
    inverse = rotation_xyz_degrees([0, 90, 0]).T @ np.array([0.0, 0.0, -1.0])
    assert np.allclose(inverse, [1, 0, 0], atol=1e-12)
    print("5 rotate_latlong checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
