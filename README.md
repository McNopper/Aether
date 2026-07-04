# Aether

The scene & material file format for the [Hyperion](https://github.com/McNopper/Hyperion)
and [Theia](https://github.com/McNopper/Theia) renderers — a small C++23
library that parses TOML scene/material files and OBJ geometry into plain CPU data structures.

> *[Aether](https://en.wikipedia.org/wiki/Aether_(mythology)) — primordial deity of the
> bright upper air and pure light the gods breathe; the medium through which light
> travels.* Aether is the **medium**: it carries a scene's description, untied to any
> renderer or GPU.

> ⚠️ **Early stage / work in progress.**

---

## What Aether is (and isn't)

Aether is **fully independent**: it has **no Vulkan, no GPU, and no renderer
dependencies** (only GLM for math and toml++ for parsing). It turns text files into plain
CPU structs; consumers (Harmonia and the renderers) own all GPU upload.

| Parses | Into |
|--------|------|
| `<name>.scene.toml` (TOML scene description) | `aether::SceneDesc` (camera, render settings, env, tonemapper, post-tonemap renderer, geometry blocks with TRS + material refs, material-library references) |
| `<name>.materials.toml` (TOML OpenPBR material library) | `aether::MaterialDesc` (OpenPBR Surface v1.1 parameters + texture references) |
| Wavefront OBJ (geometry only — **triangles only**; OBJ material directives ignored) | `aether::MeshData` / `aether::MeshGroup` (deduplicated vertices + indices, local space) |

Geometry stays in OBJ (bulk vertex/index data); every other element — scene, camera,
render settings, tonemapping, materials — is **TOML**, so files are small, single-purpose,
and easy to edit (by hand or by an agent). TOML is the best compromise between human
readability and token efficiency: compared to equivalent pretty-printed JSON, the TOML files
save up to one third of the token usage when foreign files are read by an LLM/agent — less
syntactic noise (no braces, fewer quotes), more content per token. Minified JSON would be
denser still, but is no longer readable or editable by humans — and unlike JSON, TOML
supports comments. Geometry is returned in **object/local space**;
world placement lives on the scene's geometry blocks (TRS). Colors are kept in their declared
input color space, tagged with `MaterialColorSpace`, so the consumer performs any color-space
conversion.

### Why TOML — and not OpenUSD or glTF?

OpenUSD and glTF are both excellent formats — but for different jobs than Aether's.
glTF is a *runtime delivery* format: binary buffers, a fixed metallic-roughness PBR
model (not OpenPBR), optimized for engines to load, not for humans or agents to read
and edit. OpenUSD is a powerful *composition and interchange* system (layering,
references, variants) but is heavyweight and complex — far more than a small hobby
renderer needs, and not something you hand-edit in a text box.

Aether deliberately takes the best of both and stays small and **agentic-friendly**:

* **Human- and agent-readable/editable text** (like neither format's binary payloads) —
  small, single-purpose TOML files with comments, cheap for an LLM to read and generate.
* **OBJ for bulk geometry** — the one thing that genuinely needs a compact vertex/index
  container (glTF's buffers, but simpler and ubiquitous), kept separate from the text.
* **Lightweight composition** — scene/camera/render/tonemap presets referenced and
  overridden (a small nod to USD layering) without USD's machinery.
* **OpenPBR verbatim** — material parameters use the exact OpenPBR Surface 1.1.1 names,
  rather than glTF's or USD's own material models, so nothing is lost in translation.

The result: files small enough to diff and hand-edit, structured enough to compose, and
faithful to OpenPBR — the sweet spot glTF and USD each sit to one side of.

### File-format notes

* **Scene** (`<name>.scene.toml`): top-level `material_libraries` array; `[render]`,
  `[camera]`, `[tonemap]` and `[post_tonemap]` tables; an ordered `[[geometry]]` array whose entries have
  `type = "instance" | "box" | "sphere"`. Keys are spelled in full words for clarity
  (`samples_per_pixel`, `environment_map`, `vertical_field_of_view`, `mesh`,
  `half_extents`, `material`, …). The `[render]` table may select the scene-referred
  **working color space** via `working_color_space = "lin_rec2020_scene" |
  "lin_rec709_scene"` (absent → consumer default, Rec.2020); consumers convert all
  assets to it on load.
* **Camera preset** (`<name>.camera.toml`): a small standalone preset file with
  `translate`, `look_at`, `vertical_field_of_view` and `ev100` keys. It is meant to
  be referenced from the scene file's `[camera]` section.
* **Setting presets** (`presets/<name>.<group>.toml`): the `[render]`, `[camera]` and
  `[tonemap]` and `[post_tonemap]` sections may carry a `reference = "presets/…"` key pointing at a standalone
  preset file (top-level keys, same shape as the inline section). The referenced file is
  applied first as a base, then any inline keys override it (*local wins*). Settings shared
  across scenes therefore live in a single deduplicated preset file (e.g. all Cornell scenes
  share `presets/cornell.camera.toml` and `presets/preview.render.toml`). References resolve
  relative to the scene directory.
* **Materials** (`<name>.materials.toml`): one TOML table per material (keyed by name), using
  [OpenPBR Surface v1.1](https://academysoftwarefoundation.github.io/OpenPBR/) parameter names
  verbatim; optional top-level `colorspace` and `model` (default `"openpbr"` — the material
  model tag, anticipating additional material models in the future). Texture bindings use
  Aether's `map_*` keys.
* **OBJ**: geometry only. Non-triangle faces are skipped and all `usemtl` / `mtllib`
  directives are ignored — materials are assigned exclusively from the scene file.

### Authoring tools

See `tools/README.md` for the Blender scene exporter and the MaterialX-to-TOML
converter. The Blender addon exports a `.scene.toml`, a matching `.camera.toml`
and OBJ files; OpenPBR materials stay in TOML and can be authored separately or
generated from MaterialX OpenPBR libraries.

> **Tip:** in VS Code, install the *Even Better TOML* extension for syntax highlighting,
> formatting, and (with a JSON Schema) live validation of the `.toml` files.

---

## Building

**Requirements:** CMake 3.28+, a C++23 compiler, vcpkg (provides GLM and toml++).

```bash
cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="<vcpkg-root>/scripts/buildsystems/vcpkg.cmake"

cmake --build build
```

## Tests

GoogleTest-based unit tests (pure CPU — no GPU required):

```bash
cd build && ctest --output-on-failure
```

## Consuming Aether

Hyperion, Theia and Harmonia pull Aether via CMake `FetchContent` and link the
`aether::aether` target.

---

## Dependencies

| Library | Purpose |
|---------|---------|
| [GLM](https://github.com/g-truc/glm) | Vector / quaternion math |
| [toml++](https://github.com/marzer/tomlplusplus) | TOML parsing for the `.scene.toml` / `.materials.toml` formats |
| [GoogleTest](https://github.com/google/googletest) | Unit testing (tests only) |

---

## References

The material model follows the OpenPBR Surface specification; MaterialX is the
canonical node reference used by the `tools/` MaterialX→TOML converter.

* **OpenPBR Surface v1.1.1** — Academy Software Foundation.
  <https://academysoftwarefoundation.github.io/OpenPBR/> ·
  [reference repo](https://github.com/AcademySoftwareFoundation/OpenPBR)
* **MaterialX** — specification and node library (OpenPBR `open_pbr_surface`).
  <https://materialx.org/> ·
  <https://github.com/AcademySoftwareFoundation/MaterialX>
* **Wavefront OBJ** — geometry container (triangles only; material directives ignored).