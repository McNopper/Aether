# Aether authoring tools

Helper tools for authoring and preparing Aether assets: scenes, materials and geometry. The Python
tools use the standard library only and need no build step; `aether-validate` is a small C++
program built together with the library. The file format the tools write is described in the
[repository README](../README.md#file-format-reference).

Blender has no OpenPBR surface. The intended workflow with Blender is therefore:

1. Lay out the scene in Blender and export the geometry, the scene and the camera.
2. Author the OpenPBR materials as TOML, by hand or with the MaterialX converter.
3. Reference the material library from the exported scene and render it with Theia or Hyperion.

| Tool | Purpose |
|------|---------|
| [`aether-validate`](#scene-validator-aether-validate) | Check a scene file the way the library reads it |
| [`blender_to_aether`](#blender-scene-exporter-blender_to_aether) | Export a Blender scene: scene TOML, camera preset, OBJ files, animation, skinning, morph targets |
| [`mtlx_to_aether`](#materialx-to-toml-converter-mtlx_to_aether) | Convert an OpenPBR MaterialX library to a TOML material library |
| [`obj_transform.py`](#obj-transform-obj_transformpy) | Rotate, translate or scale an OBJ file |
| [`obj_bounds.py`](#obj-bounds-obj_boundspy) | Compute the bounding box of an OBJ file as a `bounds` block |
| [`omm_bake.py`](#opacity-micromap-baker-omm_bakepy) | Bake an opacity micromap from an OBJ file and an opacity texture |
| [`test_scenes.py`](#test-scenes-test_scenespy) | Generate deterministic test scenes in headless Blender |
| [Repository checks](#repository-checks) | Architecture, dead code, clang-tidy and coverage scripts |

The scripts run with Python 3.11 or newer; `tools/omm_bake.py` needs the `tomllib` module that
Python 3.11 introduced.

## Scene validator: `aether-validate`

`aether-validate` is a thin command line over `aether::SceneParser::parse` with no validation logic
of its own. A green summary means exactly "the library accepts this file", including the checks of
the node graph, skins, morph targets and animation tracks. It is built only when Aether is the
top-level project.

```powershell
cmake --build build --target aether-validate
build\aether-validate.exe assets\cornell_classic.scene.toml        # summary line
build\aether-validate.exe assets\camera_orbit.scene.toml --tree    # plus the node hierarchy
build\aether-validate.exe assets\camera_orbit.scene.toml --all     # plus every resolved instance
```

```text
aether-validate <scene.toml> [--tree] [--all]
```

| Argument | Output |
|----------|--------|
| none | One summary line: `✔ scene OK — <n> instances, <n> meshes, <n> nodes, <n> material libraries`; a second line with the animation count follows when the scene has a `[meta]` table |
| `--tree` | The node hierarchy below the summary, two spaces of indent per level; a meshed node shows `[mesh: <name>]` |
| `--all` | Every resolved instance below the summary: index, mesh, translation, quaternion, scale and whole-mesh material (`-` when none) |

The instance list is the resolved one: flat `[[instance]]` blocks first, then one entry per meshed
node with its world transform.

| Exit code | Meaning |
|-----------|---------|
| 0 | The library accepts the file |
| 1 | The library rejected it; the `✘` line quotes the library's error message |
| 2 | Usage problem: missing or unknown argument, extra argument, or the path is not an existing regular file |

The tool reads the scene file only. It does not open the OBJ files, the material libraries or the
micromap descriptors, so a broken material reference or a wrong per-vertex row count is not caught
here; the scene loader reports those.

## Blender scene exporter: `blender_to_aether`

A Blender add-on (File → Export → **Aether Scene (.scene.toml)**) for Blender 5.2 or newer, tested
with Blender 5.2.3 and 5.3.0. One export writes into the directory of the chosen scene file:

1. `<name>.scene.toml` — the scene: one `[[mesh]]` block per unique mesh and one `[[node]]` block
   per exported object, plus, as needed, `[[skin]]`, `[[morph]]`, `[animation_clock]` and
   `[[animation]]` blocks.
2. `<name>.camera.toml` — a camera preset, when "Include Camera" is on and the scene has a camera.
   The scene references it from `[camera]`.
3. One OBJ file per unique mesh, next to the scene.

Blender materials are never converted: they are not OpenPBR and may hold arbitrary node graphs.
The exporter only writes material *names*; the materials themselves are authored in TOML.

### Install

Copy `tools/blender_to_aether/` into the Blender add-ons folder and enable *Aether Scene
Exporter*. Or run it headless:

```bash
blender --background --python-expr "import sys; sys.path.append('tools'); \
  import blender_to_aether as a; a.register(); \
  import bpy; bpy.ops.export.aether_scene(filepath='out/scene.scene.toml')"
```

### Export options

| Option | Default | Effect |
|--------|---------|--------|
| `selected_only` | off | Export only the selected objects |
| `include_camera` | on | Write the camera preset |
| `material_libraries` | empty | Comma-separated material-library files to reference |
| `render_reference` | `presets/preview.render.toml` | Render preset the scene references |
| `tonemap_reference` | `presets/aces.tonemap.toml` | Tonemap preset the scene references |
| `camera_ev100` | 12.0 | `ev100` written to the camera preset |
| `skin_blend` | `lbs` | Blend mode written to every `[[skin]]` block: `lbs` or `dqs` |
| `samples_per_pixel` | 0 | Inline `[render]` key; 0 leaves it to the preset |
| `max_depth` | 0 | Inline `[render]` key; 0 leaves it to the preset |
| `working_color_space` | `omit` | Inline `[render]` key: `lin_rec709_scene`, `lin_rec2020_scene` or `omit` |
| `environment_map` | empty | Inline `[render]` key, written as given; empty leaves it to the preset |
| `environment_unit_nits` | 0 | Inline `[render]` key; 0 leaves it to the preset |
| `tonemapper` | `omit` | Inline `[tonemap]` key: `aces`, `agx`, `reinhard`, `hable` or `omit` |
| `post_tonemap_renderer` | `omit` | Inline `[post_tonemap]` key: `green_screen` or `omit` |
| `post_tonemap_reference` | empty | `[post_tonemap]` preset reference |
| `haze_density` | 0 | Inline `[haze] density`, in metres⁻¹. Written only together with `haze_height`; 0 leaves the section out |
| `haze_height` | 0 | Inline `[haze] height`, in metres. Written only together with `haze_density`; 0 leaves the section out |
| `meta_title` | empty | `[meta]` title; empty uses the export file name without its last suffix (`camera_orbit.scene.toml` gives `camera_orbit.scene`) |
| `meta_description` | empty | `[meta]` description |

Inline keys are written only when they are set, and they override the referenced preset. Warnings
are printed to the console (`Aether export: ...`) and reported in Blender.

### Hierarchy mapping

Every exported object becomes exactly one `[[node]]`. The exporter writes no flat `[[instance]]`
blocks; the parser compounds the node graph into its flat instance list itself.

- `id` is the Blender object name. `parent` is the Blender parent's name, written only when that
  parent is exported. With `selected_only`, an object whose parent is not exported becomes a root
  node that carries the world transform, so its placement is preserved.
- The transform is the local matrix relative to the Blender parent (`matrix_local`, which includes
  Blender's parent inverse; a parentless object uses its world matrix), converted from Blender's
  Z-up frame to Aether's Y-up frame (a −90° rotation about X) and decomposed into `translate`,
  `rotate` (quaternion `[qx, qy, qz, qw]`) and `scale`. The OBJ files hold the mesh vertices in the
  same Y-up frame, so the compounded node transforms reproduce the Blender world transforms.
- A quaternion transform cannot hold shear. When recomposing the decomposed parts does not
  reproduce the matrix, a warning is printed and the closest transform is exported. This happens
  for a re-rooted `selected_only` node whose world transform contains shear (a non-uniform parent
  scale combined with a child rotation).
- A negative (mirrored) scale round-trips exactly, but it is warned about: it flips the triangle
  winding downstream.
- A mesh object additionally binds `mesh` and, when it has materials, `material` or `materials`
  (below). Objects without mesh data (empties, collection instances, lights, the scene camera)
  become grouping nodes; their children still parent to them.
- Objects that share one mesh datablock (Blender instancing) share one OBJ and one `[[mesh]]`;
  each object is a node that references the mesh. The first exported object that uses the datablock
  names the mesh. An object that carries modifiers of its own always gets its own OBJ, because its
  baked geometry differs.
- The OBJ file name, and with it the mesh name, is the object name reduced to letters, digits,
  `_`, `.` and `-`; a collision with an already used name gets a numeric suffix.
- Nodes are written parents first. The parser resolves parents by id regardless of the file order.

The OBJ export writes local geometry (the object's world transform is cleared for the export, then
restored), applies modifiers and triangulates, matching the importer's triangles-only rule.

### Material bindings

Materials are bound by name.

- An object with one material slot (or none) writes `material = "<active material>"` when it has an
  active material.
- An object with more than one slot is exported with `usemtl` lines, so that its OBJ splits into one
  sub-mesh per material. Its node carries `material` (the active material, or the first slot) and
  `materials = { "<usemtl name>" = "<material name>", ... }`, one entry for each `usemtl` name
  present in the exported OBJ. A slot that no face uses writes no `usemtl` line and gets no entry.
  The sidecar `.mtl` file that Blender writes is deleted and its `mtllib` line is removed, because
  Aether never imports OBJ material definitions.

### Animation export

Objects whose active action animates `location`, `rotation_quaternion`, `rotation_euler` or `scale`
additionally get `[[animation]]` tracks. Keys are exported one to one; the exporter never samples
or bakes.

- **Targets** are `nodes/<object name>/translation`, `.../rotation` and `.../scale`, or a single
  component of one of them (`.../<parameter>/<component>`). Tracks are written in the order
  translation, rotation, scale per object, after the `[[node]]` blocks.
- **Frames become seconds.** Blender's rate is `render.fps` frames per `render.fps_base` seconds, so
  a key's time is `frame * fps_base / fps`. The rate is written as the scene's animation clock
  (`[animation_clock] fps`), so Aether tick N is Blender frame N. A non-positive frame rate falls
  back to 24 fps with a warning.
- **Axes** are converted like the static node transforms: a translation key `(x, y, z)` becomes
  `(x, z, -y)`, a scale key becomes `(x, z, y)`, and a rotation key is converted by the same −90°
  rotation about X. The conversion is the same for every key, so it distributes over the animation
  exactly as over the node graph.
- **Interpolation.** The Blender key modes `CONSTANT`, `LINEAR` and `BEZIER` become `step`,
  `linear` and `cubic_spline`. When the per-key modes change along a curve, the curve is split into
  consecutive same-mode tracks on the same target. The Aether evaluator lets the track declared last
  for a target win at every time, and the split tracks share their boundary key, so the
  concatenation reproduces the curve exactly. The Blender easing modes (`SINE` … `BOUNCE`) have no
  equivalent: they are exported as `cubic_spline` with handle-derived tangents (exact keys,
  approximated in-between motion) and a warning.
- **Tangents.** A Blender Bézier segment becomes a Hermite curve: with `dt` the segment duration in
  seconds, the outgoing tangent of a key is `3 * (handle_right.y - co.y) / dt` and the incoming
  tangent `3 * (co.y - handle_left.y) / dt`, in value per second. Key values are always exact; the
  curve between them uses the uniform parameterization that glTF exporters use, which is exact
  when the handle x-distances are proportional to the segment.
- **Component curves that share their key frames** (the usual block-keyed case) become one vector
  track per same-mode run. A component without a curve contributes its static value. If such
  curves disagree on the per-key mode, the mode runs follow the lowest-index channel and a warning
  names the deviation.
- **Component curves keyed on different frames** become per-component scalar tracks on the
  component targets, one track per same-mode run of that component's own curve, with no
  resampling. A property is never written as a mixture of vector and component tracks.
  - `location` channels (x, y, z) drive the x, z and y components of `translation`, with the
    same swizzle as the values (Blender Y is negated into Aether Z, for values and tangents
    alike). `scale` channels drive the x, z and y components of `scale`.
  - `rotation_quaternion` channels (w, x, y, z) drive the w, x, y and z components of
    `rotation`. A quaternion component cannot be converted on its own, so a key's value is the
    converted component of the normalized Blender pose at that frame: the keyed channel contributes
    its key value and every other channel the value Blender shows there (its curve evaluated, or
    its static value). The tangents are one-sided differences of that value. Aether normalizes the
    whole quaternion after each component write, so the keys reconstruct the pose exactly whenever
    the other components stand at their Blender values.
  - A component without a curve gets no track; the node's static transform stands for it.
- **Rotation modes.** Only the curves that match the object's `rotation_mode` drive the pose.
  `QUATERNION` uses the `rotation_quaternion` channels. An Euler mode converts its `rotation_euler`
  keys to quaternion keys at the union of the three curves' key frames, without resampling; at a
  foreign frame a channel contributes the value Blender shows there. Between keys, Blender blends
  Euler angles component-wise while Aether interpolates quaternions, so key poses are exact and the
  in-between motion differs slightly. Curves of the other rotation mode do not drive the pose and
  are not exported, with a warning.
- **Single-key properties** become one constant `linear` track. Outside a track's key range both
  Blender and Aether hold the end key.
- **Not exported:** F-curve modifiers, NLA tracks, layers of an action beyond the first, drivers and
  channels other than the node transform. NLA tracks and additional layers produce a warning; the
  others are left out silently. A curve without keys or with invalid key times is skipped with a
  warning.

### Deformation export: skinning and morph targets

An object that carries an armature modifier and/or shape keys additionally gets the deformation
blocks.

- **Bones become nodes.** Every bone becomes a node with the id `<armature>::<bone>`. Root bones are
  children of the armature object's node; other bones are children of their parent bone. A bone
  node's transform is its rest transform in armature space, relative to its parent, converted to
  the Y-up frame, so the compounded node transforms reproduce the Blender rest pose.
- **`[[skin]]`** binds the meshed object: `node` is the object name, `joints` lists the bone ids in
  Blender's bone order, `inverse_bind` holds for each joint the inverse of its world rest
  transform in the Aether frame (16 row-major numbers), and `bone_indices` / `bone_weights` hold
  the four strongest influences per vertex. Vertex groups are matched to bones by name; a vertex
  group without a bone of the same name is skipped with a warning. `blend` is the export option
  `skin_blend`. Dual-quaternion skinning is rigid by design and drops scale and shear.
- **`[[morph]]`** blocks, one per shape key except the basis: `mesh`, `name` (a `/` in the key name
  is replaced, with a warning), `weight` (the key's value at export time) and `deltas`, the
  per-vertex offset from the basis key in the Y-up frame. Shape-key value curves become
  `morphs/<mesh>/<name>` tracks through the same key-for-key conversion as node tracks.

The armature object belongs in the export together with the skinned mesh: the root bones are
children of the armature's node and the inverse bind matrices are world-space. A mesh whose
modifiers include an armature is exported with its own OBJ, so its rows never mix with those of
another object.

**Import-order contract.** Aether's `ObjImporter` merges face corners with equal (position,
normal, texture coordinate) values, so a quad made of two triangles is imported as 4 vertices, not
6 corners. The exporter re-reads the OBJ file it has just written and expands every per-vertex row
in exactly that deduplicated order: per sub-mesh (`o`, `g` and `usemtl` boundaries, first occurrence
wins), with the numbers rounded to single precision as the importer sees them. This is what makes
`aether::validateSkinVertexCounts` and `aether::validateMorphVertexCounts` pass at load time.
`tests/test_blender_roundtrip.cpp` checks this contract against the committed `skinned` and
`skinned_dqs` fixtures, including a 6-versus-4 negative control.

**Texture coordinates.** The exporter writes the `vt` lines as Blender holds them: `v` is 0 at the
bottom of a picture and 1 at its top. The renderers show the first row of an image file at `v = 0`,
so a picture on a face unwrapped in Blender comes out vertically mirrored, and an OpenGL normal map
on it comes out inverted in relief. A library with such pictures stores its maps for the pipeline
as it is. Whether the exporter or the loader should flip `v` is an open decision, listed in the
[known limitations](../PLAN.md#known-limitations).

### Pose-bone animation

Curves on `pose.bones["<bone>"].location`, `.rotation_euler`, `.rotation_quaternion` and `.scale` —
the skeletal animation itself — become whole-parameter tracks on the `<armature>::<bone>` nodes.
Each key composes the pose value onto the bone's rest transform:

- translation: `t_rest + R_rest * swizzle(t_pose)`
- rotation: `q_rest * conjugate(q_pose)`
- scale: `s_rest * swizzle(s_pose)`

These maps are linear per channel group, and a left multiplication by a fixed quaternion commutes
with slerp and with the Hermite basis, so Aether's interpolation reproduces `rest * interpolated
pose` exactly. Within a parameter group, all keyed components must share their key frames and
per-key modes; an unkeyed component contributes its saved pose value. Otherwise the group is
skipped with a warning: keys stay one to one, nothing is sampled. Pose tracks are written only when
a skinned mesh of the export references the armature, because only then do the bone nodes exist.

`tests/test_blender_roundtrip.cpp` compares the exported pose tracks against vertex positions that
Blender itself evaluated (see the `skinned_pose` test scene).

### Camera

The camera preset holds `translate`, `rotate`, `vertical_field_of_view` (the camera's vertical
angle in degrees) and `ev100` (the export option). When the camera object is part of the export,
the preset also holds `node = "<camera object>"`, and the renderers place the camera from that
node's evaluated transform at every animation tick; the static transform is the load-time
fallback. A `selected_only` export without the camera writes the static transform only.

Two rules keep the aim correct:

- **A camera node's transform is rotated, not conjugated.** Every other node's local matrix is
  converted as `CONV * local * CONV⁻¹` (`CONV` is the −90° rotation about X). A camera's local
  matrix is written as `CONV * local` instead. A camera's payload is its optical axis: the
  conjugation would swizzle the local axes too (local −Z would land on −Y), while the renderers
  read the view direction as `forward = rotation * (0, 0, -1)`. With the rotated form the
  compounded camera transform comes out as `CONV * R` (same position, correct aim). The
  `camera_orbit` fixture pins this: the camera at position (0, 2, 6) looks at the origin at ticks
  0 and 58.
- **Camera rotation keys take the same rotated form**, so a directly animated camera stays aimed
  as well. A camera parented to another camera is not supported.

The renderer flag `--frame N` sets the tick at which playback or capture starts.

### Scene-level output

**Settings.** The options `samples_per_pixel`, `max_depth`, `working_color_space`,
`environment_map` and `environment_unit_nits` are written as inline `[render]` keys,
`tonemapper` as `[tonemap] tonemapper`, `post_tonemap_renderer` / `post_tonemap_reference` into
`[post_tonemap]`, and `haze_density` / `haze_height` into `[haze]`. Inline keys override the presets.

**Haze.** The format takes both `[haze]` keys or neither. When one of `haze_density` and
`haze_height` is above 0 and the other is not, the exporter reports it in its warnings
(`[haze] needs both density and height; only one is set, so no haze is exported`) and writes no
`[haze]` section, because a scene with one key is rejected. The two values are written with the
shortest decimal that parses back to the single-precision number Blender holds, so a scene reads
exactly what was set.

**Mesh bounds.** Every OBJ mesh block gets `bounds = { min = [...], max = [...] }`, the
object-space bounding box read back from the exported OBJ file after the axis conversion and the
modifiers. The renderers use it for culling instead of scanning the vertices.

**Node context.** Two custom properties on an object are written to its node: `description`
(string) becomes `description`, and `tags` (a comma-separated string or a list) becomes `tags`.
They never affect rendering.

**Metadata and link tables.** A scene with content gets a `[meta]` table with `generator`
(`blender_to_aether/<version>`), `title` and, when set, `description`. Each exported OBJ gets a
`[meta.link."<path>"]` table: `kind = "mesh"`, the triangle count, the group count (the number of
`usemtl` lines, at least 1), the object-space `bounds` with `frame = "object"`, and the object's
`description` when set. An explicitly given `environment_map` gets a `kind = "env"` link and every
`material_libraries` entry a `kind = "material-library"` link. An environment map that comes from a
preset is not linked, so the tables do not depend on the directory the scene is exported into.

**Units.** Geometry is exported unscaled and Aether scenes are in metres. The exporter inspects
the scene's unit system and scale length, but its warning is added after the warnings have been
reported, so the warning is not shown: a scene in imperial units or with a scale length other than
1 exports silently at the wrong scale.

### What the exporter does not write

| Format feature | Why |
|----------------|-----|
| `type = "box"`, `type = "sphere"` | Analytic shapes are authored by hand; Blender geometry always exports as a real OBJ |
| `opacity_micromaps` | Produced by `tools/omm_bake.py` and assembled in the scene by hand |
| Flat `[[instance]]` blocks | Every object becomes a `[[node]]`; the parser resolves the same flat list |
| `[meta]` `schema`, `version`, `model`, `units`, `assembly` | The parser's defaults hold for an export (`aether-scene`, `1`, `openpbr`, `m`, empty); `title` and `description` are the two keys the export writes |

## MaterialX to TOML converter: `mtlx_to_aether`

Converts an OpenPBR MaterialX library (`.mtlx`) into an Aether material library
(`<name>.materials.toml`). MaterialX documents are XML; the converter parses the XML directly with
`xml.etree.ElementTree` and needs neither the MaterialX Python bindings nor a native library.

```bash
python tools/mtlx_to_aether/mtlx_to_toml.py input.mtlx [output] [--colorspace lin_rec709_scene|lin_rec2020_scene] [--force]
```

| Argument | Meaning |
|----------|---------|
| `input` | The `.mtlx` file |
| `output` | Output file, or a directory (a path that exists as a directory or ends with a slash); the file is then named after the input. Omitted: `<input name>.materials.toml` next to the input |
| `--colorspace` | Overrides the library's `colorspace` |
| `--force` | Overwrite an existing output file; without it an existing file is an error |

Exit code 0 on success, 1 when the input is missing, is not OpenPBR, cannot be parsed or the output
exists. The converter prints one line with the number of materials and the colour space.

What it converts:

- Each `open_pbr_surface` node that is a direct child of the document root becomes a material, named
  after the `surfacematerial` that references it, else after the node name.
- Literal inputs are written under their OpenPBR names: scalars, three-component colours and the
  two Booleans. The output starts with `model = "openpbr"` and `colorspace`: the document's
  `colorspace` attribute becomes `lin_rec2020_scene` when it contains `2020`, otherwise
  `lin_rec709_scene`.
- An input connected to an `image` node (a direct child of the root, by `nodename` or `nodegraph`)
  becomes a texture map with its colour space: `srgb_rec709_scene` for `base_color` and
  `emission_color`, `data` for the rest, unless the image declares one. Texture paths are rewritten
  relative to the output file. The input-to-slot mapping: `base_color` → `map_base_color`;
  `specular_roughness` → `map_roughness`; `base_metalness` → `map_metalness`; `geometry_normal` →
  `map_normal`; `geometry_coat_normal` → `map_coat_normal`; `geometry_tangent` → `map_tangent`;
  `geometry_coat_tangent` → `map_coat_tangent`; `emission_color` → `map_emission_color`;
  `orm` (or `occlusion_roughness_metalness`) → `map_orm`. The short names `roughness`,
  `metalness`, `normal`, `coat_normal`, `tangent` and `coat_tangent` are accepted as well.
- An input the converter does not know is dropped without a message. In particular a textured
  `geometry_opacity` produces no `map_opacity`.

**OpenPBR only.** A document without an `open_pbr_surface` node is rejected. When it uses another
surface model (`standard_surface`, `gltf_pbr`, `disney_principled`, `usd_preview_surface`) the
error names it. Standard Surface, OpenPBR's predecessor, is what most third-party libraries still
ship; reliable OpenPBR sources are the MaterialX example library and the OpenPBR reference
repository.

### Tests

```bash
python tools/mtlx_to_aether/tests/run_tests.py
```

For each OpenPBR fixture in `tools/mtlx_to_aether/tests/fixtures/` the converter regenerates the
library and compares it byte for byte with the golden file of the same name in
`tools/mtlx_to_aether/tests/expected/`. The fixture whose name starts with `not_openpbr` must be
rejected. 17 fixtures: 16 golden conversions and one rejection; exit code 0 when all pass.

### Examples

`tools/mtlx_to_aether/examples/shader_ball.mtlx` and `tools/mtlx_to_aether/examples/bunny.mtlx`
are OpenPBR libraries whose material names match the five zones of `assets/shader_ball.obj` and the
`assets/bunny.obj` scene. Converting them gives drop-in replacements for
`assets/shader_ball.materials.toml` and `assets/bunny_shaderball.materials.toml`:

```bash
cd tools/mtlx_to_aether
python mtlx_to_toml.py examples/shader_ball.mtlx examples/shader_ball_openpbr.materials.toml --force
python mtlx_to_toml.py tests/fixtures/open_pbr_carpaint.mtlx out.materials.toml
```

(`--force` is needed in the first command because the committed example output already exists.)

### MaterialX's official OpenPBR examples

The ten official MaterialX OpenPBR examples (the OpenPbr folder under
resources/Materials/Examples of the MaterialX repository) are gathered in this repository:
default, aluminum_brushed (anisotropy), carpaint (coat), glass (dispersion), honey (transmission
scatter), ketchup (subsurface), lightbulb (emission), pearl (thin film, subsurface, coat),
soapbubble (thin film, thin-walled) and velvet (fuzz). Each is a flat document: an
`open_pbr_surface` root, literal inputs and no node graphs. All ten convert without a dropped
parameter.

| In the repository | Content |
|-------------------|---------|
| `tools/mtlx_to_aether/tests/fixtures/open_pbr_*.mtlx` and their goldens | The ten sources, as conversion tests |
| `assets/openpbr_mtlx.materials.toml` | The ten materials merged into one library, under their upstream names |
| `assets/openpbr_mtlx.scene.toml` | A showcase with the ten materials on spheres |

`Glass`, `Pearl` and `Velvet` also exist in other asset libraries. A material of the same name in a
later-loaded library replaces the earlier one, so a scene must not load both libraries.

## OBJ transform: `obj_transform.py`

Rotates, translates or scales a Wavefront OBJ file and writes a transformed copy. It rewrites the
`v` lines (and, for rotations, the `vn` lines) and copies every other line unchanged. It preserves
the file's encoding, line endings and numeric precision. When input and output are the same file,
the result is written to a temporary file in the same directory and moved over the input, so an
in-place edit never truncates the source.

```text
python tools/obj_transform.py rotate    <x|y|z> <90|180|270> <input.obj> <output.obj>
python tools/obj_transform.py translate <x|y|z> <offset>          <input.obj> <output.obj>
python tools/obj_transform.py scale     <factor>                    <input.obj> <output.obj>
```

| Command | Effect |
|---------|--------|
| `rotate <axis> <angle>` | Rotates positions and normals by 90, 180 or 270 degrees about the axis. Only axes are swapped and signs flipped, on the original number tokens, so every value survives byte for byte and four 90° steps are an exact identity |
| `translate <axis> <offset>` | Adds an offset to one axis of every position; normals, texture coordinates and faces are untouched |
| `scale <factor>` | Multiplies every position by one factor; normals, texture coordinates and faces are untouched |

```bash
python tools/obj_transform.py translate y input.obj -0.21 output.obj   # offset Y by -0.21
python tools/obj_transform.py scale    input.obj 0.1   output.obj      # uniform scale by 0.1
python tools/obj_transform.py rotate   y 90    input.obj output.obj    # +90 degrees about Y
python tools/obj_transform.py rotate   y 90    input.obj input.obj     # in place
```

Exit code 0 on success, 2 when the angle is not 90, 180 or 270, the offset or factor is not a
number, or a line to transform holds a token that is not a number. Lines with fewer than three
coordinates are copied unchanged. The streaming engine lives in the shared module
`tools/_obj_transform.py`.

### Tests

```bash
python tools/test_obj_transform.py
```

Covers the nine rotations on `v` and `vn` lines, translate and scale goldens, the byte-exact
4 × 90° and 2 × 180° identities, CRLF preservation, extra vertex-colour columns, short `v` lines,
safe in-place edits and the exact validation messages and exit codes.

## OBJ bounds: `obj_bounds.py`

Computes the object-space axis-aligned bounding box of an OBJ file's vertices and prints it as an
Aether `bounds` block. The box can be written into the `[[mesh]]` entry of a scene so that the
renderers can cull against it without scanning the vertices.

```bash
python tools/obj_bounds.py assets/shader_ball.obj                                   # block on stdout
python tools/obj_bounds.py assets/shader_ball.obj --write assets/shader_ball.scene.toml  # inject into the scene
python tools/obj_bounds.py assets/shader_ball.obj --sphere                           # plus a bounding sphere
```

| Argument | Effect |
|----------|--------|
| `input` | The OBJ file |
| `--write PATH` | Replace or add `[mesh.bounds]` under the `[[mesh]]` entry whose `path` names the OBJ file, and rewrite the scene atomically |
| `--sphere` | Also compute a Ritter bounding sphere (`center`, `radius`) and print or write it as `[sphere]` / `[mesh.sphere]`; the scene parser ignores the sphere table |
| `--pretty` | Align the `=` signs in the emitted TOML |
| `--quiet` | Suppress the summary lines |

Output: a summary on stderr (`<file>: <n> vertices, AABB min=(...) max=(...)`), the TOML block on
stdout (or `wrote [mesh.bounds] into <scene>` with `--write`). The block looks like this:

```toml
# object-space bounds for shader_ball.obj (12345 vertices)
[bounds]
min = [-13.46, 0.21, -13.46]
max = [13.46, 26.78, 11.55]
```

Only `v` lines are read; `vn`, `vt` and `f` lines are ignored, because the bounding volume of the
vertex set is the bounding volume of the mesh. Numbers are printed with ten significant digits.
Exit code 0 on success, 1 when the input or the scene file does not exist, the OBJ has no vertex,
or no `[[mesh]]` entry matches.

### Tests

```bash
python tools/test_obj_bounds.py
```

Covers the AABB computation, the TOML block round trip, the exclusion of `vn`, `vt` and `f` lines,
the empty-OBJ rejection, the Ritter sphere enclosure and the `--write` injection.

## Opacity micromap baker: `omm_bake.py`

Bakes an opacity micromap (`VK_KHR_opacity_micromap`) from an OBJ file and the opacity texture of
a material, producing the Aether asset pair that `aether::OmmImporter` reads: a `.omm` text sidecar
and a `.micromap.toml` descriptor. The micromap is the bake product of the mesh's UVs and the
material's `map_opacity`; the scene references it per OBJ group on the mesh. There is no alpha-test
threshold anywhere in the chain: the opacity is a presence weight, and the bake records a definite
state only where the opacity is provably 0 or 1.

The format of the two files is described in the
[README](../README.md#opacity-micromaps).

```bash
# scene-driven: bake the groups whose material declares map_opacity and inject the references
python tools/omm_bake.py --scene assets/shaderball_checker.scene.toml --mesh shader_ball_omm --write

# explicit: bake given groups from one material's map_opacity
python tools/omm_bake.py assets/shader_ball.obj \
    --material-lib assets/shaderball_checker.materials.toml \
    --material CheckerBall --groups BallSurface,BaseFoot

# write an NxM-cell checkerboard opacity PNG
python tools/omm_bake.py --emit-checker 256 256 8 8 assets/checker_opacity.png

# re-bake from the recorded sources and compare
python tools/omm_bake.py --check assets/shaderball_checker_omm.micromap.toml

# print one triangle's sub-triangle states
python tools/omm_bake.py --inspect assets/shaderball_checker_omm.micromap.toml --group BallSurface --triangle 0
```

| Argument | Effect |
|----------|--------|
| `<obj>` | Input OBJ (explicit mode) |
| `--scene TOML`, `--mesh NAME` | Scene-driven mode |
| `--material-lib TOML`, `--material NAME` | Explicit mode |
| `--groups G1,G2` | Groups to bake (required in explicit mode; a filter in scene mode) |
| `--level N` | Subdivision level, default 2: `4^N` sub-triangles per triangle |
| `--format 1` or `--format 2` | State format, default 2 (four states). 1 is two states |
| `--out-omm PATH`, `--out-toml PATH` | Output paths. Default: `<stem>_omm.omm` and `<stem>_omm.micromap.toml` in the scene's directory (scene mode) or the OBJ's directory; the stem is the scene name without `.scene.toml`, or the OBJ name |
| `--write` | Inject `opacity_micromaps` into the scene's `[[mesh]]` entry (scene mode only) |
| `--emit-checker W H CELLSU CELLSV OUT` | Write a checkerboard opacity PNG and exit |
| `--check TOML` | Re-bake from a descriptor's recorded sources and compare with the stored sidecar |
| `--inspect TOML --group NAME --triangle N` | Print the sub-triangle states of one base triangle with their space-filling-curve indices |
| `--quiet` | Suppress progress and summary lines |

**Scene-driven mode** reads the scene's first material library and the first `[[instance]]` block
of the named mesh. The keys of that instance's `materials` map are the group names; a group is
baked when its material declares `map_opacity`. `[[node]]` blocks are not read, so a scene that
places its mesh through a node (every Blender export) needs a hand-written `[[instance]]` or the
explicit mode. Group names are the OBJ's `o`/`g` names.

**How a triangle is classified.** For each sub-triangle the baker takes the range of the opacity
texture over the sub-triangle's whole UV footprint (its texel-space bounding box grown by one
bilinear tap), multiplied by the material's `geometry_opacity`, and samples with the renderer's
convention (wrap-repeat, no V flip):

| Opacity range | Four states | Two states |
|---------------|-------------|-------------|
| everywhere 0 | transparent | transparent |
| everywhere 1 | opaque | opaque |
| mixed | unknown-opaque (traversal asks the shader) | transparent |

Four states is the default because the two unknown states make the micromap an accelerator rather
than a second, coarser definition of the cut-out: a sub-triangle that straddles a cut-out edge is
handed back to the renderer's exact per-hit test, so a micromapped mesh renders identically to the
same mesh without one. A triangle whose sub-triangles all share a state collapses to a `S` line. A
triangle without texture coordinates becomes `S -4` (unknown-opaque). The opacity of a texel is its
alpha channel when the image has one, else its Rec.601 luminance.

The baker's OBJ reader triangulates polygons by fan; the importer skips non-triangles.

**Outputs.** `<name>.omm` holds one `G <group>` section per baked group and, per base triangle in
OBJ face order, one `T <level> <format> <hex>` or `S <special>` line. `<name>.micromap.toml` holds
`format = "omm-text/v1"`, the recorded sources (`source_obj`, `source_scene`, `source_material_library`,
`source_texture`) for `--check`, the defaults, the `[data]` sidecar reference and one `[[group]]`
table with `name`, `source_material`, `triangle_count`, `record_count` and the `usage` histogram.
Progress and the per-group summary go to stderr:

```text
  BaseFoot: 10024 tris -> 3505 records + 6519 special [L2F2x3505]
```

**`--check`** re-bakes every recorded group from the recorded OBJ, texture and material library and
compares each triangle with the stored sidecar. It reports the first 20 differences and exits with
1 on any difference, 0 with `in sync with sources`. **`--inspect`** prints the level, format and
one `index : state` line per sub-triangle.

Exit codes: 0 success, 1 error (an argument problem, a missing file or group, a material without
`map_opacity`, a malformed sidecar) or drift, 2 when Python is older than 3.11.

### Tests

```bash
python tools/test_omm_bake.py
```

Covers PNG encode and decode, the checkerboard, the space-filling-curve index (every sub-triangle
index is hit exactly once per level), the least-significant-bit-first packing against hand-computed
bytes, the collapse to special states, a full synthetic bake, the `.omm` round trip, the OBJ group
attribution and fan triangulation, the scene `--write` injection and the failure modes.

## Test scenes: `test_scenes.py`

Builds a deterministic test scene in headless Blender, exports it through the add-on and writes an
expectations file next to it. The committed fixtures in `assets/` are produced this way and read by
`tests/test_blender_roundtrip.cpp`.

```text
blender --background --python tools/test_scenes.py -- --scene <name> --out <dir>
```

Each run writes `<scene>.scene.toml`, the OBJ files, the camera preset and `<scene>.expected.json`
(the Blender world matrices of the meshed nodes; for the pose scene, Blender's evaluated vertex
positions at the listed frames). Repeated runs of the same scene give the same bytes.

| Scene | Covers |
|-------|---------|
| `hierarchy` | A three-level parent chain of empties, a shared mesh under different parents, a negative-scale grouping node, a parentless mesh |
| `animated` | Constant and Bézier keys, a rotation, a scale ramp, a mixed-mode location curve (split into same-mode tracks) |
| `simple`, `instancing` | A single cube; one mesh datablock referenced by two objects |
| `skinned`, `skinned_dqs` | A two-bone armature over a quad with blocky vertex-group weights and a `Smile` shape key keyed over frames 1 to 30; the two differ in `skin_blend` (`lbs` and `dqs`) |
| `skinned_pose` | Pose-bone animation: the root bone's location and the child bone's rotation keyed |
| `camera_orbit`, `skinned_orbit`, `camera_static` | A camera bound to a node: on an orbiting pivot, together with the skinned fixture, and without any animation |
| `feature_matrix` | Every scene-level export option, a two-slot object with `usemtl` groups, node `description` and `tags`, computed bounds and link tables |

The three camera scenes take the `meadow_64spp_8bounce` render preset and switch the camera export
on, because a factory-empty scene has no lights and the direct-light preset would render black.

## Python tests

Each Python tool has a self-contained test script (standard library only, temporary directory,
exit code 0 on success):

```bash
python tools/mtlx_to_aether/tests/run_tests.py
python tools/test_obj_transform.py
python tools/test_obj_bounds.py
python tools/test_omm_bake.py
```

They are not registered with CTest.

## Repository checks

Four scripts support the C++ side of the repository. The commands in context are in the
[README](../README.md#testing).

| Script | Purpose | Exit codes |
|--------|---------|------------|
| `tools/check_arch.py` | Checks the include graph of `src/` against the contract in `ARCHITECTURE.md` | 0 clean, 1 violations, 2 configuration or usage error, 77 skipped |
| `tools/check_dead.py` | Finds orphan headers, sources that are never compiled and `FetchContent_Declare` names that are never consumed | 0 clean, 1 findings, 2 usage error, 77 nothing to scan |
| `tools/check_tidy.py` | Runs clang-tidy over `build/compile_commands.json` in parallel | 0 clean, 1 error-category findings, 77 clang-tidy or database missing |
| `tools/coverage.py` | Builds the instrumented tree, runs the tests and prints the line coverage of `src/` | 0 report produced, 1 failure, 77 LLVM tools missing, 2 usage or configuration problem |

```text
python tools/check_arch.py [--repo DIR] [--doc FILE] [--report] [--json] [--diagram] [--derive [--scan DIRS] [--out FILE]] [--selftest] [--version]
python tools/check_dead.py [--repo DIR] [--family DIR] [--report] [--version]
python tools/check_tidy.py [--db PATH] [--jobs N]
python tools/coverage.py [--build DIR] [--skip-build] [--show]
```

### `check_arch.py`

The document is the contract: the rules live in the `arch` block of `ARCHITECTURE.md`, and the
script extracts the include graph of the sources and compares the two (see
[ARCHITECTURE.md](../ARCHITECTURE.md)). A run prints `<n> architecture violation(s):` followed by
one line per violation; a clean run ends with `architecture clean (...)`, naming what was checked.

| Argument | Effect |
|----------|--------|
| `--repo DIR` | Repository to check (default: the directory above the script) |
| `--doc FILE` | Contract document (default: `ARCHITECTURE.md` at the root) |
| `--report` | Also print the layer stack and the measured module graph, including the report-only smells |
| `--json` | Print the summary as JSON |
| `--diagram` | Regenerate the diagram section of `ARCHITECTURE.md` from the rules; a stale diagram section fails every check |
| `--derive [--scan DIRS] [--out FILE]` | Draft an `ARCHITECTURE.md` from the measured include graph, written to a file or to stdout |
| `--selftest` | Run the engine's own tests on synthetic fixtures in a temporary directory |

### `check_dead.py`

Three checks, each printed as `PASS` or `FAIL` with a count: `orphan-headers` (headers that no
file includes), `never-compiled` (`.cpp` files that are in no compile database and that no CMake
file names) and `fetchcontent` (declared but never consumed dependencies). With `--family DIR`, a
header that has no consumer in this repository is looked up in the sibling repositories under the
directory; a header that a sibling includes is reported as information and is no finding.

A finding needs a look before anything is deleted. A justified finding is listed in
`tools/check_dead.exceptions`, one entry per line: `<check> <path-or-name> <reason>`. Real dead
code is deleted instead.

### `check_tidy.py`

Runs clang-tidy over every translation unit of `src/` and `tests/` in `build/compile_commands.json`
(default database; `--db` overrides; `--jobs` sets the parallelism, by default the CPU count minus
two and at least four).
It locates clang-tidy through the newest Visual Studio installation (vswhere), so it works on
Windows. Findings are classified like `.clang-tidy` does: `clang-diagnostic-*`, `clang-analyzer-*`
and `bugprone-*` fail the run; `modernize-*`, `performance-*` and `portability-*` are counted in a
summary. The full output is written to `build/clang_tidy.log`.

### `coverage.py`

Needs a configure with `-DAETHER_COVERAGE=ON` (a separate build directory keeps the main build
untouched). Without `--skip-build` the script configures and builds the tree itself, in
`build/coverage` by default, reusing the generator, compiler and toolchain of the main build. It
then runs the unit tests with `LLVM_PROFILE_FILE` set, merges the profiles with `llvm-profdata` and
prints the `llvm-cov report` of `src/` for every test binary that CTest lists (`--show` prints the
annotated sources instead). The LLVM tools are looked up next to the compiler first.

## References

- **OpenPBR Surface** — Academy Software Foundation.
  <https://academysoftwarefoundation.github.io/OpenPBR/> ·
  [reference repository](https://github.com/AcademySoftwareFoundation/OpenPBR)
- **MaterialX** — specification and node library (OpenPBR node `open_pbr_surface`).
  <https://materialx.org/> · <https://github.com/AcademySoftwareFoundation/MaterialX>
- **Vulkan opacity micromaps** — `VK_KHR_opacity_micromap` (promoted from
  `VK_EXT_opacity_micromap`); the baker reproduces the reference mapping of sub-triangle indices.
