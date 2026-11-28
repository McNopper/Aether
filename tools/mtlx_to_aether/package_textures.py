"""Package converted Aether libraries with bounded textures, preserving 16-bit PNG/TIFF.

The input is the output directory of ``mtlx_to_toml.py``: ``materials/`` plus
``manifest.json``. Every referenced image is converted to PNG, resized with Lanczos
in its encoded/sampled representation, and deduplicated by resolved source path.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import tomllib
import zlib
from pathlib import Path

import numpy as np
from PIL import Image

from mtlx_graph import save_png16


def png_header(path: Path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    width, height, depth, color, _compression, _filtering, interlace = struct.unpack(
        ">IIBBBBB", data[16:29]
    )
    return width, height, depth, color, interlace, data


def read_png16(path: Path) -> np.ndarray:
    """Read a non-interlaced 16-bit PNG without Pillow's RGB16-to-RGB8 conversion."""
    width, height, depth, color, interlace, data = png_header(path)
    if depth != 16 or interlace != 0:
        raise ValueError(f"unsupported 16-bit PNG layout: {path}")
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(color)
    if channels is None:
        raise ValueError(f"unsupported PNG color type {color}: {path}")
    idat = bytearray()
    offset = 8
    while offset < len(data):
        size = struct.unpack(">I", data[offset : offset + 4])[0]
        kind = data[offset + 4 : offset + 8]
        payload = data[offset + 8 : offset + 8 + size]
        if kind == b"IDAT":
            idat.extend(payload)
        offset += 12 + size
    raw = zlib.decompress(bytes(idat))
    row_bytes = width * channels * 2
    bpp = channels * 2
    expected = height * (row_bytes + 1)
    if len(raw) != expected:
        raise ValueError(f"PNG scanline size mismatch: {path}: {len(raw)} != {expected}")
    rows = np.empty((height, row_bytes), dtype=np.uint8)
    cursor = 0
    for y in range(height):
        filter_type = raw[cursor]
        cursor += 1
        source = np.frombuffer(raw[cursor : cursor + row_bytes], dtype=np.uint8)
        cursor += row_bytes
        row = np.empty(row_bytes, dtype=np.uint8)
        prior = rows[y - 1] if y else np.zeros(row_bytes, dtype=np.uint8)
        for x in range(row_bytes):
            left = int(row[x - bpp]) if x >= bpp else 0
            up = int(prior[x])
            upper_left = int(prior[x - bpp]) if x >= bpp else 0
            value = int(source[x])
            if filter_type == 0:
                predictor = 0
            elif filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = up
            elif filter_type == 3:
                predictor = (left + up) // 2
            elif filter_type == 4:
                p = left + up - upper_left
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upper_left)
                predictor = left if pa <= pb and pa <= pc else (up if pb <= pc else upper_left)
            else:
                raise ValueError(f"unknown PNG filter {filter_type}: {path}")
            row[x] = (value + predictor) & 255
        rows[y] = row
    values = np.frombuffer(rows.tobytes(), dtype=">u2").astype(np.uint16)
    return values.reshape(height, width, channels)


def load_source(path: Path):
    """Return ``(H,W,C)`` uint8/uint16 pixels and the source precision."""
    if path.suffix.lower() == ".png":
        header = png_header(path)
        if header and header[2] == 16:
            return read_png16(path), 16
    with Image.open(path) as image:
        image.load()
        width, height = image.size
        if image.mode.startswith("I;16"):
            dtype = ">u2" if image.mode == "I;16B" else "<u2"
            values = np.frombuffer(image.tobytes(), dtype=dtype).astype(np.uint16)
            return values.reshape(height, width, 1), 16
        if image.mode not in {"L", "LA", "RGB", "RGBA"}:
            image = image.convert("RGBA")
        values = np.asarray(image, dtype=np.uint8).reshape(height, width, -1)
        return values.copy(), 8


def resize_array(array: np.ndarray, max_dimension: int) -> np.ndarray:
    if max_dimension <= 0:
        raise ValueError("max_dimension must be positive")
    height, width = array.shape[:2]
    scale = min(1.0, max_dimension / max(width, height))
    if scale == 1.0:
        return array
    target = (max(1, round(width * scale)), max(1, round(height * scale)))
    if array.dtype == np.uint16:
        channels = []
        for index in range(array.shape[2]):
            plane = Image.fromarray(array[..., index].astype(np.int32), mode="I")
            resized = plane.resize(target, Image.Resampling.LANCZOS)
            channels.append(np.clip(np.asarray(resized), 0, 65535).astype(np.uint16))
        return np.stack(channels, axis=2)
    image = Image.fromarray(array[..., 0] if array.shape[2] == 1 else array)
    resized = image.resize(target, Image.Resampling.LANCZOS)
    values = np.asarray(resized, dtype=np.uint8)
    return values.reshape(target[1], target[0], -1).copy()


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def package(source: Path, output: Path, max_dimension: int = 1024) -> dict:
    """Package one converted directory and return the manifest dictionary."""
    if max_dimension <= 0:
        raise ValueError("max_dimension must be positive")
    source, output = source.resolve(), output.resolve()
    if source == output:
        raise ValueError("source and output directories must differ")
    materials_out, textures_out = output / "materials", output / "textures"
    materials_out.mkdir(parents=True, exist_ok=True)
    textures_out.mkdir(parents=True, exist_ok=True)

    records = {}
    reference_count = 0
    material_names = {}
    for library in sorted((source / "materials").glob("*.materials.toml")):
        document = tomllib.loads(library.read_text(encoding="utf-8"))
        text = library.read_text(encoding="utf-8")
        names = []
        for material, table in document.items():
            if not isinstance(table, dict):
                continue
            names.append(material)
            for key, value in table.items():
                if not key.startswith("map_") or key.endswith("_colorspace") or not isinstance(value, str):
                    continue
                reference_count += 1
                original = (library.parent / value).resolve()
                if original not in records:
                    stem = original.stem + "_preview"
                    candidate = stem + ".png"
                    used = {r["target"] for r in records.values()}
                    if candidate in used:
                        candidate = stem + "_" + sha256(original)[:8] + ".png"
                    array, bits = load_source(original)
                    array = resize_array(array, max_dimension)
                    target = textures_out / candidate
                    if bits == 16:
                        save_png16(target, array)
                    else:
                        image = Image.fromarray(array[..., 0] if array.shape[2] == 1 else array)
                        image.save(target, format="PNG", compress_level=4)
                    records[original] = {
                        "source": str(original),
                        "target": candidate,
                        "bits": bits,
                        "width": int(array.shape[1]),
                        "height": int(array.shape[0]),
                        "channels": int(array.shape[2]),
                        "bytes": target.stat().st_size,
                        "sha256": sha256(target),
                    }
                target_name = records[original]["target"]
                old = f'{key} = "{value}"'
                new = f'{key} = "../textures/{target_name}"'
                if old not in text:
                    raise RuntimeError(f"cannot rewrite {library.name}: {old}")
                text = text.replace(old, new, 1)
        (materials_out / library.name).write_text(text, encoding="utf-8")
        material_names[library.name] = names

    shutil.copyfile(source / "manifest.json", output / "conversion_manifest.json")
    problems = []
    for record in records.values():
        if max(record["width"], record["height"]) > max_dimension:
            problems.append(f"oversize: {record['target']}")
    payload = {
        "source": str(source),
        "max_dimension": max_dimension,
        "material_libraries": len(material_names),
        "material_variants": sum(len(v) for v in material_names.values()),
        "texture_references": reference_count,
        "unique_textures": len(records),
        "high_precision": sum(r["bits"] == 16 for r in records.values()),
        "total_bytes": sum(r["bytes"] for r in records.values()),
        "materials": material_names,
        "textures": list(records.values()),
        "problems": problems,
    }
    (output / "manifest.json").write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--max-dimension", type=int, default=1024)
    args = parser.parse_args()
    payload = package(args.source, args.output, args.max_dimension)
    print("PACKAGE_SUMMARY", json.dumps({k: v for k, v in payload.items() if k not in {"materials", "textures"}}))
    return 1 if payload["problems"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
