# Aether authoring tools

Helper tools for producing Aether scene and material files. Both are plain
Python (standard library only) and have **no build step**.

Because Blender does not yet author OpenPBR natively, the intended workflow is:

1. Model/lay out the scene in Blender → export geometry + scene + camera.
2. Author (or convert) the OpenPBR materials as TOML separately.
3. Reference the material library from the exported scene file and render.

---

## 1. Blender scene exporter (`aether_blender_exporter`)

A Blender addon (File → Export → **Aether Scene (.scene.toml)**) that writes:

1. `<name>.scene.toml` — one `[[geometry]]` `instance` block per mesh object
   (TRS from the object's world transform), an optional `material_libraries`
   list, and `[render]` / `[camera]` / `[tonemap]` references.
2. `<name>.camera.toml` — a camera preset (`translate`, `look_at`,
   `vertical_field_of_view`, `ev100`) referenced from the scene's `[camera]`
   section.
3. One `.obj` per mesh object (geometry only; **materials are not exported**).

It deliberately does **not** convert Blender materials to OpenPBR — Blender has
no OpenPBR surface yet, so materials are authored in TOML (by hand or via the
converter below) and referenced from the scene file.

**Install:** copy `aether_blender_exporter/` into your Blender addons folder and
enable *Aether Scene Exporter*, or run it headless:

```bash
blender --background --python-expr "import sys; sys.path.append('tools'); \
  import aether_blender_exporter as a; a.register(); \
  import bpy; bpy.ops.export.aether_scene(filepath='out/scene.scene.toml')"
```

**Export options:** `selected_only`, `include_camera`, `material_libraries`
(comma-separated), `render_reference`, `tonemap_reference`, `camera_ev100`.

---

## 2. MaterialX → TOML converter (`mtlx_to_toml_converter`)

Converts an OpenPBR MaterialX (`.mtlx`) file into an Aether
`<name>.materials.toml`.

MaterialX documents are plain XML, so the converter **parses the XML directly**
(`xml.etree.ElementTree`) and has **no dependency on the MaterialX Python
bindings** or any native library.

```bash
python tools/mtlx_to_toml_converter/mtlx_to_toml.py input.mtlx output.materials.toml
python tools/mtlx_to_toml_converter/mtlx_to_toml.py input.mtlx out/ --colorspace lin_rec2020_scene
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
python tools/mtlx_to_toml_converter/tests/run_tests.py
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

## References

* **OpenPBR Surface v1.1.1** — Academy Software Foundation.
  <https://academysoftwarefoundation.github.io/OpenPBR/>
* **OpenPBR reference repository** — AcademySoftwareFoundation/OpenPBR.
  <https://github.com/AcademySoftwareFoundation/OpenPBR>
* **MaterialX** specification and node library (OpenPBR node
  `open_pbr_surface`, `libraries/bxdf/open_pbr_surface.mtlx`).
  <https://materialx.org/> · <https://github.com/AcademySoftwareFoundation/MaterialX>
