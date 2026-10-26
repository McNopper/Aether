# Aether authoring tools

Helper tools for authoring and preparing Aether assets — scenes, materials and
geometry. All are plain Python (standard library only) and have **no build step**.

Because Blender does not yet author OpenPBR natively, the intended workflow is:

1. Model/lay out the scene in Blender → export geometry + scene + camera.
2. Author (or convert) the OpenPBR materials as TOML separately.
3. Reference the material library from the exported scene file and render.

---

## 1. Blender scene exporter (`blender_to_aether`)

A Blender addon for **Blender 5.2 or newer** (`bl_info` declares
`blender = (5, 2, 0)`; File → Export → **Aether Scene (.scene.toml)**) that
writes:

1. `<name>.scene.toml` — one `[[mesh]]` block per unique mesh (name + `.obj`
   path) and one `[[node]]` block per exported object (`id`, optional
   `parent`, a `translate` / `rotate` / `scale` TRS, and for mesh objects the
   `mesh` reference plus material binding), an optional `material_libraries`
   list, and `[render]` / `[camera]` / `[tonemap]` references. The Aether
   parser validates the node graph (duplicate ids, unknown parents, cycles)
   and compounds the parent transforms into its flat instance list itself.
2. `<name>.camera.toml` — a camera preset (`translate`, `rotate`,
   `vertical_field_of_view`, `ev100`) referenced from the scene's `[camera]`
   section.
3. One `.obj` per unique mesh (geometry only; **materials are not exported**).

It deliberately does **not** convert Blender materials to OpenPBR — Blender has
no OpenPBR surface yet, so materials are authored in TOML (by hand or via the
converter below) and referenced from the scene file.

### Hierarchy mapping (NH4)

The exporter emits the NH1 node graph. **Design decision: every object becomes
exactly one `[[node]]` — a single code path, no flat `[[instance]]` fallback**
(hand- or tool-written `[[instance]]` blocks still parse in Aether, they are
just no longer emitted by this exporter).

* `id` = the Blender object name; `parent` = the Blender parent's name, but
  **only when that parent is itself exported** (it always is in full-scene
  exports). With `selected_only`, a selected object whose parent is not
  exported is re-rooted: the `parent` key is dropped and the node carries the
  world transform instead, so placement is preserved.
* Transform: the local matrix relative to the Blender parent
  (`obj.matrix_local`, which includes Blender's parent inverse; parentless
  objects use `matrix_world`), conjugated Blender Z-up → Aether Y-up
  (`CONV @ M @ CONV⁻¹`) and decomposed into `translate` / `rotate`
  (quaternion `[qx,qy,qz,qw]`) / `scale`. The conjugation distributes over
  composition, so the parser's per-level T×R×S compounding reproduces the
  Blender world transforms. Shear is not representable in a quaternion TRS:
  when the decomposed parts do not round-trip the matrix — this can happen
  for a re-rooted `selected_only` node whose Blender world contains shear
  (non-uniform parent scale × child rotation) — a warning is printed and the
  nearest TRS is exported, a documented limitation. A negative (mirrored)
  scale round-trips exactly but is warned about, because it flips triangle
  winding downstream.
* Mesh objects additionally bind `mesh` (the `[[mesh]]` name) and, when the
  object has an active material, `material` — exactly the instance semantics.
* Objects sharing one mesh datablock (Blender instancing) export **one**
  OBJ/`[[mesh]]` and become multiple nodes referencing that one mesh name;
  Aether instances the mesh per node. The first exported object using the
  data names the mesh. A data-shared object carrying its own modifiers always
  gets its own OBJ, because the baked geometry would differ.
* Objects without mesh data — empties, collection instances (which are
  empties), lights, the scene camera — become **meshless grouping nodes**;
  children still parent to them. `scene.objects` already spans all
  collections, so collection membership needs no extra mapping.
* Nodes are emitted parents-first (stable topological order); the parser
  resolves parents by id regardless of file order.

### Animation export (NH4b)

Objects whose **active action** drives `location` / `rotation_quaternion` /
`rotation_euler` / `scale` additionally emit `[[animation]]` tracks (ANI1
schema, `src/aether/format/SceneAnimator.hpp`) after the `[[node]]` blocks,
key for key — the exporter **never samples or bakes**. Objects without an
active action (or an action that drives none of those channels) emit none; a
scene without animation is unchanged from the pre-NH4b output.

* **Targets**: `nodes/<object name>/translation`, `.../rotation`,
  `.../scale` (the object name is the `[[node]]` `id`). One scalar Blender
  F-curve is ONE channel of a vector track. Emission order per object is
  translation → rotation → scale; segment runs of one target stay
  consecutive.
* **Frames → seconds**: ANI1 times are seconds. Blender's frame-rate model is
  `render.fps` frames per `render.fps_base` seconds, so the effective rate is
  `fps / fps_base` and `time = frame · fps_base / fps` (negative frames give
  negative times — both finite, both valid). The schema has no fps field and
  none is invented; the exporter writes the used rate as a TOML comment above
  the first `[[animation]]` and this paragraph is the contract.
* **Frames → axes**: animated values are exported in the Aether frame exactly
  like the static node TRS: a translation key `(x, y, z)` becomes
  `(x, z, -y)`, a scale key `(x, y, z)` becomes `(x, z, y)`, and a rotation
  key `q` is conjugated `Q_CONV ⊗ q ⊗ Q_CONV⁻¹` (the quaternion form of the
  same -90° X rotation). Since conjugation is fixed, it distributes over the
  animation exactly as over the static node graph. Quaternion products are
  spelled out in Python doubles on purpose — mathutils' Quaternion products
  are float32 in Blender 5.x, and keys conjugate exactly.
* **Interpolation**: per-key `CONSTANT`/`LINEAR`/`BEZIER` map to
  `step`/`linear`/`cubic_spline`. **Per-key variance splits the F-curve** into
  consecutive same-mode `[[animation]]` entries on the same target. The
  Aether evaluator gives a later-declared track precedence from its first key
  onwards; split boundaries share their boundary key (identical time and
  value in both tracks), so the concatenation reproduces the F-curve exactly
  and the distinct key set is preserved (per-track key counts therefore sum
  to `keys + runs − 1`). Other easing modes (`SINE` … `BOUNCE`) have no ANI1
  equivalent: they export as `cubic_spline` with the handle-derived tangents —
  keys exact, in-between approximated — with a warning.
* **Tangents** (cubic_spline keys): a Blender BEZIER segment is the cubic
  Bézier over `(co, handle_right | handle_left, co)`; as a Hermite over the
  segment duration `dt` (seconds) that is
  `m_out = 3·(handle_right.y − co.y) / dt` and
  `m_in = 3·(co.y − handle_left.y) / dt` — value-per-second derivatives, the
  form the ANI1 evaluator multiplies by `dt`. Rotation tangents conjugate
  exactly like the values (the product is bilinear; euler-channel rates are
  converted through the derivative of the euler→quaternion map). Key values
  are always exact; the segment shape is the glTF-style uniform-
  parameterization Hermite, exact when handle x-deltas are proportional to
  the segment and the same approximation every glTF exporter makes against
  Blender ≥ 3.0's x-parameterized Bézier evaluation. A channel whose stored
  handle sits on its key (freshly keyed boundary keys headless) exports a
  zero tangent — faithful to what Blender itself evaluates.
* **Registry limit (component key-time mismatch)**: the ANI1 target registry
  has only the three vector parameters and a target parameter may not contain
  `/` (`splitAnimationTarget`), so per-component scalar tracks
  (`nodes/<id>/translation/x`) do not parse today. A property is therefore
  exported only when **all its component F-curves share identical key
  frames** (the block-keyed common case; a missing component contributes its
  static value, which is exact — Blender evaluates a channel without an
  F-curve as its static value). When component key frames differ, the
  property cannot be represented 1:1 without resampling (forbidden): the
  animation of that property is **not exported**, a warning names it, and the
  node's static TRS stands. Scalar component targets in the registry are a
  tracked follow-up. If component curves share key frames but disagree on
  per-key interpolation, the mode segments follow the lowest-index channel
  (values at keys stay exact; a warning names the deviation).
* **Rotation modes**: only the curves matching the object's `rotation_mode`
  drive the pose. `QUATERNION` uses the `rotation_quaternion` channels
  (Blender order w, x, y, z → schema x, y, z, w). An Euler mode converts its
  `rotation_euler` keys (in the object's rotation order) to quaternion keys
  at the **union** of the three curves' key frames — key count preserved, no
  resampling: at a foreign frame a channel contributes the value Blender
  itself shows there (its F-curve evaluated and clamped, or its static value
  without a curve). In-between, Blender blends euler channels component-wise
  before converting while ANI1 slerps quaternions: key poses are exact,
  in-between motion differs slightly (schema property, same for block-keyed
  quaternion curves). Curves on the non-matching rotation path (e.g. stale
  `rotation_euler` curves on a quaternion object) do not drive the pose and
  are not exported (warning).
* **Single-key** properties export one constant `linear` track (ANI1: one key
  holds everywhere — the same constant Blender's clamped evaluation shows);
  outside a track's key range both Blender and ANI1 clamp to the endpoint
  key. The node's static TRS remains the export-time pose and is replaced per
  parameter only where tracks drive it.
* **Not exported**: F-curve modifiers (`CYCLE` etc.), NLA layering and
  multi-layer (layered) actions beyond the first layer/active slot, drivers,
  and channel data paths other than the node TRS — each warning when present;
  the active action's plain F-curves are the 1:1 contract.

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

## 3. OBJ transform CLI (`obj_transform.py`)

One CLI for the per-axis Wavefront OBJ transforms (standard library only). It reads an
OBJ and writes a transformed copy, operating on the `v` (vertex) and, for rotations,
`vn` (normal) lines. It preserves file encoding, line endings and numeric precision,
and is **safe for in-place edits** — when `input.obj` and `output.obj` resolve to the
same file, the result is written through a temp file in the same directory and
atomically `os.replace`d, so the source is never truncated mid-read.

* `translate <x|y|z> <offset>` — add a numeric offset to one axis of every vertex.
* `scale <factor>` — multiply every vertex by a uniform factor (normals, UVs and
  faces are left untouched).
* `rotate <x|y|z> <90|180|270>` — rotate by **90, 180 or 270°** about an axis using
  exact coordinate swizzling (no trig): axes are only swapped and signs flipped on
  the original string tokens, so each value is preserved byte-for-byte and a
  4×90° round-trip is an exact identity.

```bash
python tools/obj_transform.py translate y input.obj -0.21 output.obj   # offset Y by -0.21
python tools/obj_transform.py scale    input.obj 0.1   output.obj     # uniform scale x0.1
python tools/obj_transform.py rotate   y 90    input.obj output.obj   # +90 deg about Y
python tools/obj_transform.py rotate   y 90    input.obj input.obj    # in-place (safe)
```

The streaming engine (one input OBJ → one output OBJ, `v`/`vn` lines rewritten,
everything else copied byte-for-byte) lives in the shared `_obj_transform.py`
module, also used by `obj_bounds.py`.

### Tests

```bash
python tools/test_obj_transform.py
```

Golden-file tests (stdlib only): the nine rotate swizzles on `v`+`vn`, translate/
scale goldens, 4×90°/2×180° byte-exact identities, CRLF preservation, extra
vertex-color columns and short `v` lines, safe in-place edits, and the exact
validation messages/exit codes.

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

Bakes a Vulkan `VK_KHR_opacity_micromap` (OMM) from a Wavefront OBJ plus a
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

## Test scenes (NH4 round-trip)

`tools/test_scenes.py` builds deterministic test scenes in headless Blender
and exports them through the addon - the NH4 verification oracle:

```text
blender --background --python tools/test_scenes.py -- \
        --scene hierarchy|animated|simple|instancing --out <dir>
```

Each run writes `<scene>.scene.toml` plus `<scene>.expected.json`
(Blender-computed world matrices per meshed node) - the oracle the
round-trip verification compares against. `animated` exercises the
NH4b contract: CONSTANT blocks, BEZIER tangents, and a
mixed-interpolation location curve split into per-mode tracks.

## References

* **OpenPBR Surface v1.1.1** — Academy Software Foundation.
  <https://academysoftwarefoundation.github.io/OpenPBR/>
* **OpenPBR reference repository** — AcademySoftwareFoundation/OpenPBR.
  <https://github.com/AcademySoftwareFoundation/OpenPBR>
* **MaterialX** specification and node library (OpenPBR node
  `open_pbr_surface`, `libraries/bxdf/open_pbr_surface.mtlx`).
  <https://materialx.org/> · <https://github.com/AcademySoftwareFoundation/MaterialX>
