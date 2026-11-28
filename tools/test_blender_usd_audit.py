"""Headless Blender test for blender_usd_audit.py using an in-memory USD stage."""
from pathlib import Path
import sys

from pxr import Gf, Sdf, Usd, UsdGeom, UsdShade

sys.path.insert(0, str(Path(__file__).resolve().parent))
from blender_usd_audit import audit_stage

stage = Usd.Stage.CreateInMemory()
UsdGeom.Xform.Define(stage, "/World")
mesh = UsdGeom.Mesh.Define(stage, "/World/textured")
material = UsdShade.Material.Define(stage, "/World/Looks/mat")
UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(material)
mesh.GetPrim().CreateAttribute("primvars:arnold:visibility", Sdf.ValueTypeNames.Int).Set(0)
st = UsdGeom.PrimvarsAPI(mesh.GetPrim()).CreatePrimvar(
    "st", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.faceVarying
)
st.Set([Gf.Vec2f(0, 0), Gf.Vec2f(1, 0), Gf.Vec2f(1, 1), Gf.Vec2f(0, 1)])
plane = UsdGeom.Mesh.Define(stage, "/World/matte")
plane.GetPrim().CreateAttribute("primvars:isMatteObject", Sdf.ValueTypeNames.Bool).Set(True)

result = audit_stage(stage)
assert result["mesh_count"] == 2
assert result["hidden_meshes"] == 1
assert result["matte_meshes"] == ["/World/matte"]
assert result["unbound_meshes"] == ["/World/matte"]
textured = result["meshes"][0]
assert textured["bound_material"] == "mat"
assert textured["uv_count"] == 4 and textured["uv_interpolation"] == "faceVarying"
print("6 blender_usd_audit checks passed")
