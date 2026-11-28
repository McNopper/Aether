"""Audit USD geometry/material metadata through Blender's bundled OpenUSD.

Run: blender -b --python tools/blender_usd_audit.py -- input.usda output.json
The JSON records composed mesh bindings, UV counts, Arnold visibility chains,
matte flags, light inputs and camera projection parameters. No scene is imported,
modified or rendered.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from pxr import Usd, UsdGeom, UsdShade


def _value(prim, name):
    attr = prim.GetAttribute(name)
    return attr.Get() if attr and attr.HasValue() else None


def audit_stage(stage) -> dict:
    meshes = []
    for prim in stage.Traverse():
        if not prim.IsA(UsdGeom.Mesh):
            continue
        hidden = False
        chain = []
        cursor = prim
        while cursor:
            value = _value(cursor, "primvars:arnold:visibility")
            chain.append([cursor.GetPath().pathString, value])
            hidden |= value == 0
            cursor = cursor.GetParent()
        material, relationship = UsdShade.MaterialBindingAPI(prim).ComputeBoundMaterial()
        material_prim = material.GetPrim() if material else None
        st = UsdGeom.PrimvarsAPI(prim).GetPrimvar("st")
        coordinates = st.GetAttr().Get() if st and st.GetAttr().HasValue() else []
        meshes.append({
            "path": prim.GetPath().pathString,
            "name": prim.GetName(),
            "bound_material_path": material_prim.GetPath().pathString if material_prim else None,
            "bound_material": material_prim.GetName() if material_prim else None,
            "binding_relationship": str(relationship) if relationship else None,
            "arnold_hidden_in_chain": hidden,
            "usd_visibility": str(UsdGeom.Imageable(prim).ComputeVisibility()),
            "visibility_chain": chain,
            "matte": bool(_value(prim, "primvars:isMatteObject") or 0),
            "uv_count": len(coordinates),
            "uv_interpolation": str(st.GetInterpolation()) if st else None,
        })

    lights = []
    light_types = {"DistantLight", "SphereLight", "RectLight", "DiskLight", "CylinderLight", "DomeLight", "GeometryLight"}
    for prim in stage.Traverse():
        if prim.GetTypeName() not in light_types and not any("LightAPI" in s for s in prim.GetAppliedSchemas()):
            continue
        inputs = {
            attr.GetName(): str(attr.Get())
            for attr in prim.GetAttributes()
            if attr.GetName().startswith("inputs:") and attr.HasValue()
        }
        lights.append({"path": prim.GetPath().pathString, "type": prim.GetTypeName(), "inputs": inputs})

    cameras = []
    for prim in stage.Traverse():
        if not prim.IsA(UsdGeom.Camera):
            continue
        camera = UsdGeom.Camera(prim)
        cameras.append({
            "path": prim.GetPath().pathString,
            "focal_length_mm": _value(prim, "focalLength"),
            "horizontal_aperture_mm": _value(prim, "horizontalAperture"),
            "vertical_aperture_mm": _value(prim, "verticalAperture"),
            "clipping_range": str(_value(prim, "clippingRange")),
            "projection": str(camera.GetProjectionAttr().Get()),
        })

    return {
        "meters_per_unit": UsdGeom.GetStageMetersPerUnit(stage),
        "up_axis": str(UsdGeom.GetStageUpAxis(stage)),
        "mesh_count": len(meshes),
        "hidden_meshes": sum(m["arnold_hidden_in_chain"] for m in meshes),
        "matte_meshes": [m["path"] for m in meshes if m["matte"]],
        "unbound_meshes": [m["path"] for m in meshes if not m["bound_material"]],
        "meshes": meshes,
        "lights": lights,
        "cameras": cameras,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else None)
    stage = Usd.Stage.Open(str(args.input))
    if not stage:
        raise RuntimeError(f"cannot open USD stage: {args.input}")
    result = audit_stage(stage)
    result["source"] = str(args.input.resolve())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(f"USD_AUDIT meshes={result['mesh_count']} hidden={result['hidden_meshes']} output={args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
