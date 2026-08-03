# Aether authoring tools

Helper tools for authoring and preparing Aether assets — scenes, materials and
geometry. All are plain Python (standard library only) and have **no build step**.

Because Blender does not yet author OpenPBR natively, the intended workflow is:

1. Model/lay out the scene in Blender → export geometry + scene + camera.
2. Author (or convert) the OpenPBR materials as TOML separately.
3. Reference the material library from the exported scene file and render.

---

## 1. Blender scene exporter (`blender_to_aether`)

A Blender addon (File → Export → **Aether Scene (.scene.toml)**) that writes:

1. `<name>.scene.toml` — a `[[mesh]]` block (name + `.obj` path) and a matching
   `[[instance]]` block (TRS from the object's world transform, plus material)
   per mesh object, an optional `material_libraries` list, and `[render]` /
   `[camera]` / `[tonemap]` references.
2. `<name>.camera.toml` — a camera preset (`translate`, `rotate`,
   `vertical_field_of_view`, `ev100`) referenced from the scene's `[camera]`
   section.
3. One `.obj` per mesh object (geometry only; **materials are not exported**).

It deliberately does **not** convert Blender materials to OpenPBR — Blender has
no OpenPBR surface yet, so materials are authored in TOML (by hand or via the
converter below) and referenced from the scene file.

**Install:** copy `blender_to_aether/` into your Blender addons folder and
enable *Aether Scene Exporter*, or run it headless:

```bash
blender --background --python-expr "import sys; sys.path.append('tools'); \
  import blender_to_aether as a; a.register(); \
  import bpy; bpy.ops.export.aether_scene(filepath='out/scene.scene.toml')"
```

**Export options:** `selected_only`, `include_camera`, `material_libraries`
(comma-separated), `render_reference`, `tonemap_reference`, `camera_ev100`.

---

## 2. MaterialX → TOML converter (`mtlx_to_aether`)

Converts an OpenPBR MaterialX (`.mtlx`) file into an Aether
`<name>.materials.toml`.

MaterialX documents are plain XML, so the converter **parses the XML directly**
(`xml.etree.ElementTree`) and has **no dependency on the MaterialX Python
bindings** or any native library.

```bash
python tools/mtlx_to_aether/mtlx_to_toml.py input.mtlx output.materials.toml
python tools/mtlx_to_aether/mtlx_to_toml.py input.mtlx out/ --colorspace lin_rec2020_scene
```

It preserves the Aether material-library contract (see
`src/aether/format/MaterialLibrary.cpp` and `src/aether/types/MaterialDesc.hpp`):

* top-level `model = "openpbr"` and `colorspace = "lin_rec709_scene" |
  "lin_rec2020_scene"` (derived from the MaterialX document `colorspace`);
* one TOML table per material, keyed by the `surfacematerial` name;
* scalar/colour parameters use the **exact OpenPBR Surface 1.1.1 input names**;
* image-connected inputs become Aether `map_*` bindings with a
  `map_*_colorspace` (sRGB for colour maps, `data` otherwise).

**OpenPBR only.** Non-OpenPBR surface models (Autodesk **Standard Surface**,
glTF PBR, UsdPreviewSurface, Disney principled) are rejected — Standard Surface
is the older predecessor that OpenPBR supersedes and uses a different node
schema. Most third-party libraries (e.g. AMD GPUOpen) still ship Standard
Surface, so today the reliable OpenPBR sources are the MaterialX example
library and the OpenPBR reference repository (see references).

### Tests

Golden-file tests (no framework required):

```bash
python tools/mtlx_to_aether/tests/run_tests.py
```

Fixtures live in `tests/fixtures/` (diffuse, metal, glass, coat, emissive,
textured, and a Standard-Surface rejection case) with expected output in
`tests/expected/`.

### Examples

`examples/shader_ball.mtlx` and `examples/bunny.mtlx` are OpenPBR libraries whose
material names match the `shader_ball.obj` (5 zones) and `bunny.obj` scenes in
`assets/`. Converting them produces drop-in replacements for
`assets/shader_ball.materials.toml` / `assets/bunny.materials.toml`:

```bash
python mtlx_to_toml.py examples/shader_ball.mtlx examples/shader_ball_openpbr.materials.toml
```

---

## 3. OBJ transform tools (`translate_obj_*`, `rotate_obj_*`, `scale_obj`)

Per-axis Wavefront OBJ transform scripts (standard library only). Each reads an
OBJ and writes a transformed copy, operating on the `v` (vertex) and, for
rotations, `vn` (normal) lines. They preserve file encoding, line endings and
numeric precision, and are **safe for in-place edits** — when `input.obj` and
`output.obj` resolve to the same file, the result is written through a temp file
in the same directory and atomically `os.replace`d, so the source is never
truncated mid-read.

* `translate_obj_x.py` / `_y.py` / `_z.py` — add a numeric offset to one axis
  of every vertex.
* `scale_obj.py` — multiply every vertex by a uniform factor (normals, UVs and
  faces are left untouched).
* `rotate_obj_x.py` / `_y.py` / `_z.py` — rotate by **90, 180 or 270°** about an
  axis using exact coordinate swizzling (no trig): axes are only swapped and
  signs flipped on the original string tokens, so each value is preserved
  byte-for-byte and a 4×90° round-trip is an exact identity.

```bash
python tools/translate_obj_y.py input.obj -0.21 output.obj   # offset Y by -0.21
python tools/scale_obj.py        input.obj 0.1   output.obj   # uniform scale x0.1
python tools/rotate_obj_y.py     input.obj 90    output.obj   # +90 deg about Y
python tools/rotate_obj_y.py     input.obj 90    input.obj    # in-place (safe)
```

---

## 4. OBJ bounds generator (`obj_bounds.py`)

Computes the object-space axis-aligned bounding box (AABB) of a Wavefront OBJ's
vertices and emits it as an Aether `bounds` block, which can be baked onto a
`[[mesh]]` entry so the renderer has an authorable bounding volume (used by
Theia's GPU-driven frustum cull and, in future, the wavefront ray-generation
phase). Standard library only.

Streams the OBJ as bytes and dispatches on the `v` line prefix (matching
`_obj_transform.py`'s reader); `vn`/`vt`/`f` lines are ignored — the vertex
set's bounds are the mesh's bounds. Numbers are emitted with `%.10g` to stay
free of floating-point noise.

```bash
# print the AABB as a TOML block to stdout (pipeable)
python tools/obj_bounds.py assets/shader_ball.obj

# inject/replace the [bounds] under the matching [[mesh]] entry in-place
python tools/obj_bounds.py assets/shader_ball.obj --write assets/shader_ball.scene.toml

# also compute a Ritter bounding sphere
python tools/obj_bounds.py assets/shader_ball.obj --sphere
```

The bounding volume of the vertex set equals the bounding volume of the mesh,
so faces are irrelevant — and OBJ winding (which matters for rendering) is not
read here either.

### Tests

```bash
python tools/test_obj_bounds.py
```

Golden-file tests (stdlib only): basic AABB, TOML block round-trip, `vn`/`vt`/`f`
ignored, empty-OBJ rejection, Ritter sphere enclosure, and `--write` injection.

---

---

## 5. Opacity micromap baker (`omm_bake.py`)

Bakes a Vulkan `VK_EXT_opacity_micromap` (OMM) from a Wavefront OBJ plus a
material's opacity texture, producing the Aether OMM asset pair consumed
downstream by Harmonia (which builds a `VkMicromapEXT` and chains it into the
per-group BLAS). Standard library only (`zlib` + `struct` + `tomllib`).

**Dependency model.** The micromap is the bake product of `(mesh UVs) ×
(material map_opacity)` — the same implicit coupling every textured material
already carries (its textures are authored against the mesh's UV layout). The
opacity input lives on the **material** (`map_opacity`, multiplied by the scalar
`geometry_opacity`, mirroring the `map_base_color` / scalar-`base_color` pattern);
the baked OMM cache is referenced per-group on the **mesh**
(`opacity_micromaps`, since a BLAS is built once per mesh, not per instance).
There is no alpha-test threshold anywhere in the chain — OpenPBR's α is a
presence weight, and the bake only ever records where it is provably 0 or 1.

**Outputs (OBJ-companion style — TOML describes structure, a text sidecar holds
the per-triangle records):**

* `<name>.omm` — OBJ-style text sidecar. One `G <group>` section per baked
  group; within, `T <level> <format> <hex>` is one base triangle's micromap
  record (the hex is the dump of the packed-state bytes, LSB-first in the
  recursive space-filling-curve order defined by the Vulkan/glTF reference
  `BarycentricsToSpaceFillingCurveIndex`), and `S <special>` collapses a
  fully-uniform triangle (`-1` transparent / `-2` opaque / `-3` unknown-transparent
  / `-4` unknown-opaque) to no record.
* `<name>.micromap.toml` — descriptor: parameters, the `[data]` sidecar
  reference, recorded sources (`source_obj` + `source_material_library` +
  `source_texture`) for staleness `--check`, and one `[[group]]` table with the
  `VkMicromapUsageEXT` usage histogram per group.

```bash
# scene-driven (recommended): auto-discovers the hero groups whose material
# declares map_opacity, bakes them, and injects the reference into the scene.
python tools/omm_bake.py --scene assets/shaderball_checker.scene.toml \
    --mesh shader_ball --write

# explicit: bake given groups from a named material's map_opacity
python tools/omm_bake.py assets/shader_ball.obj \
    --material-lib assets/shaderball_checker.materials.toml \
    --material CheckerBall --groups BallSurface,BaseFoot

# emit an NxM-cell RGBA checkerboard opacity PNG
python tools/omm_bake.py --emit-checker 256 256 8 8 assets/checker_opacity.png

# re-bake from the recorded sources and diff (mesh x material drift check)
python tools/omm_bake.py --check assets/shaderball_checker_omm.micromap.toml

# dump one triangle's per-microtriangle states with their SFC indices
python tools/omm_bake.py --inspect assets/shaderball_checker_omm.micromap.toml \
    --group BallSurface --triangle 0
```

Defaults: `--level 2` (4² = 16 microtriangles/triangle), `--format 2` (4-state,
2 bits/microtriangle). Four-state is the default because it carries the two
*unknown* states — "traversal cannot decide, ask the shader" — which is what
makes the micromap an accelerator rather than a second, coarser definition of
the cutout: microtriangles that straddle a cut-out edge are handed back to the
renderer's exact per-hit `geometry_opacity * map_opacity` test (any-hit in
Hyperion, RayQuery candidate in Theia). Each microtriangle is classified from
the **range** of opacity over its whole UV footprint (bounding box grown by the
bilinear tap, matching the renderer's `VK_FILTER_LINEAR` / `REPEAT` sampler and
its no-V-flip UV convention), so a micromapped mesh renders identically to the
same mesh without one.

### Tests

```bash
python tools/test_omm_bake.py
```

Golden tests (stdlib only): PNG encode/decode round-trip, checker pattern, the
SFC-index bijection (every microtriangle index hit exactly once per level),
LSB-first hex packing (with hand-computed golden bytes), special-index
collapse, a full synthetic bake with a popcount invariant that is independent of
the SFC mapping, the `.omm` parse round-trip, OBJ group attribution /
fan-triangulation, the scene `--write` injection (adds + replaces, TOML-valid),
and the failure modes (material without `map_opacity`, malformed hex length).

---

## References

* **OpenPBR Surface v1.1.1** — Academy Software Foundation.
  <https://academysoftwarefoundation.github.io/OpenPBR/>
* **OpenPBR reference repository** — AcademySoftwareFoundation/OpenPBR.
  <https://github.com/AcademySoftwareFoundation/OpenPBR>
* **MaterialX** specification and node library (OpenPBR node
  `open_pbr_surface`, `libraries/bxdf/open_pbr_surface.mtlx`).
  <https://materialx.org/> · <https://github.com/AcademySoftwareFoundation/MaterialX>
