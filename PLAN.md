# PLAN — Aether

This file lists the outstanding work and the known limitations of the file-format library, the
release rules, the measured baselines and the release history. Finished work is not tracked here:
a finished feature is described in the [README](README.md), and its release is recorded in
[RELEASE_NOTES.md](RELEASE_NOTES.md) and in `git log`. The family-wide order of work is in
`Harmonia/PLAN.md`.

## Roadmap

### What is not done

| Item | State |
|------|-------|
| OpenPBR Surface 1.2 | Aether conforms to OpenPBR Surface 1.1.2. The 1.2 minor specification is a separate track that has not started; versions older than 1.1.2 are not supported. Parameter naming follows OpenPBR and MaterialX |
| Animation loop and wrap | Tracks hold their end key outside the key range. Looping and wrapping are reserved and not implemented |
| Metadata for material libraries | Material libraries have no metadata block of their own. A link summary in a scene lists material names only; a one-line description per material is not available |
| Opacity micromaps from Blender | The Blender exporter does not write `opacity_micromaps`. The baker produces the descriptor and the sidecar; the mesh reference is assembled in the scene by hand |

Nothing else is queued in Aether. The next format item is the OpenPBR Surface 1.2 track. The
renderers' use of the format (deformation on the GPU, animated cameras, sequence capture) is
planned in the sibling repositories.

### Known limitations

File format:

- A node's transform compounds translation × rotation × scale per level. A non-uniform scale on a
  parent combined with a rotated child is approximated: the rotation is kept and the scale factors
  are multiplied per axis.
- Aether records mesh and material references but does not check them. A node or instance that
  names an undeclared mesh, or a `materials` key that names no sub-mesh, is reported by the scene
  loader, not by the parser.
- Skin and morph rows are checked against the vertex count at load time, not at parse time.
  Procedural spheres have no fixed vertex list and can carry neither a skin nor a morph target.
- Dual-quaternion skinning is rigid by design: it drops scale and shear.
- Morph targets move positions only; normals are not recomputed.
- A `cubic_spline` rotation track interpolates components and normalizes the result; it is not a
  spherical spline.
- Opacity micromaps are addressed by the `o`/`g` name of a sub-mesh and need
  `VK_KHR_opacity_micromap`; Harmonia fails the scene load when the device lacks it (there is no
  opaque fallback).
- Harmonia uploads eight of the ten texture maps. `map_roughness` and `map_metalness` are parsed
  and recorded only.

Blender exporter (details in [tools/README.md](tools/README.md#blender-scene-exporter-blender_to_aether)):

- Shear is not representable in a quaternion transform: the closest transform is exported, with a
  warning. A negative scale is exported as it is and warned about, because it flips triangle
  winding.
- F-curve modifiers, NLA tracks, layers of an action beyond the first and drivers are not exported.
  Blender's easing modes are approximated as cubic splines. Euler keys are exported as quaternions,
  so the motion between keys differs slightly from Blender's.
- A camera parented to another camera is not supported.
- The armature of a skinned mesh has to be part of the export: root bones are children of the
  armature's node and the inverse bind matrices are world-space.
- Geometry is exported unscaled and Aether scenes are in metres. The exporter inspects the scene's
  unit system and scale length, but its warning is added after the warnings have been reported, so
  a scene in imperial units or with a scale length other than 1 exports without a notice.

MaterialX converter (details in [tools/README.md](tools/README.md#materialx-to-toml-converter-mtlx_to_aether)):

- Only flat documents convert: literal inputs and image nodes that are direct children of the root.
  An unknown input is dropped without a message; a textured `geometry_opacity` produces no
  `map_opacity`.
- `thin_film_thickness` is copied as written. The MaterialX examples carry 0.42 (Pearl) and 0.5
  (Soap_Bubble); OpenPBR's default of 0.5 corresponds to Aether's default of 500 nanometres.

Opacity-micromap baker (details in [tools/README.md](tools/README.md#opacity-micromap-baker-omm_bakepy)):

- The scene-driven mode reads the first material library and the first `[[instance]]` block of the
  mesh. A scene that places its mesh through a `[[node]]` (every Blender export) needs explicit
  mode or a hand-written instance.
- The baker names groups by the OBJ's `o`/`g` statements and fan-triangulates polygons; the
  importer drops polygons instead.

Tooling:

- `tools/check_tidy.py` finds clang-tidy through the Visual Studio installation and works on
  Windows only.

## Baselines

Numbers a change must not regress:

| Measure | Value | What it measures |
|---------|-------|------------------|
| `ctest` | 170 of 170 pass (after v1.0.0) | 167 GoogleTest cases of the CPU suite plus the clang-tidy, architecture and dead-code checks |
| Line coverage of `src/` | 83.48 % (measured at v0.7.7) | `tools/coverage.py` over the CPU suite; a reference value, no automated threshold |
| MaterialX converter tests | 17 fixtures pass | 16 golden conversions and one rejection |
| Blender fixtures | Deterministic export | Repeated runs of `tools/test_scenes.py` for one scene give the same bytes; the committed fixtures in `assets/` are its output (tested with Blender 5.2.3 and 5.3.0, headless) |
| Scenes in the repository | Every `*.scene.toml` in `assets/` parses | The test `SceneParser.AllShippedScenesParse` |

## Definition of done

A change is done when the tests pass and the build is warning-clean. Warnings are errors
(`/W4 /WX` for MSVC-style compilers, `-Wall -Wextra -Werror -Wpedantic` for Clang and GCC); a warning
is fixed at its cause and never silenced. A release additionally passes `verify-full`: build,
tests, `format-check`, `clang-tidy` and cppcheck.

Defects are fixed in the code that owns them as soon as they are known; deferring a known defect
is not an accepted resolution.

## Release rules

- Releases follow the dependency direction: slang-math → **Aether** → Harmonia → Theia and
  Hyperion. Downstream repositories bump their FetchContent pins in their own release commits.
- Tags are pushed to GitHub. After a local tag, `git ls-remote --tags origin` must match
  `git tag -l`.
- Work is committed per repository; a push happens only on explicit request.

## Release history

Details of the recent releases are in [RELEASE_NOTES.md](RELEASE_NOTES.md); earlier history is in
`git log`.

| Version | Date | Content | Tests |
|---------|------|---------|-------|
| Unreleased | — | A sub-mesh keeps both its object/group name and its `usemtl` name, and a scene's material key may use either, so a Blender export with several materials per object renders each part with its material. The Blender exporter writes a binding only for the materials that exported faces use. The scene `motion_blur` for the motion-blur comparison between the renderers | 170 |
| v1.0.0 | 2026-11-26 | Stable baseline: analyzer cleanup in scene parsing and skinning, strict documentation-reference checks. Project version and slang-math pin 1.0.0 | 168 |
| v0.12.0 | 2026-11-24 | Family version alignment for Harmonia's shared camera assembly; no source change | |
| v0.11.0 | 2026-11-23 | `constexpr` keyword tables, binding layout, designated initializers and explicit index types | |
| v0.10.0 | 2026-11-20 | Documentation aligned with real file names; slang-math pin v0.10.0 | |
| v0.9.0 | 2026-11-16 | Deformation schema: skinning (linear and dual-quaternion) and morph targets with their CPU references; Blender exporter 1.6.0 (pose-bone animation, skin, morph, camera bound to a node, bounds, link tables); the MaterialX OpenPBR examples | 150 |
| v0.8.1 | 2026-10-30 | slang-math pin v0.8.0 (additive API) | |
| v0.8.0 | 2026-10-27 | Core format: node graph, animation tracks and tick clock, node descriptions and tags, the assembly note, the scene validator | 100 |
| v0.7.8 | 2026-10-17 | The scene `[meta]` block, one OBJ transform CLI, keyword dispatch tables for the material parser, material weight clamps, blocking coverage job | 45 |
| v0.7.7 | 2026-10-16 | The architecture check in its current form (`tools/check_arch.py`), the coverage script, code cleanup | 44 |
| v0.7.6 | 2026-10-15 | The unused umbrella header removed; a first layer check | 44 |
| v0.7.5 | 2026-10-09 | CI on Windows and Linux with the LFS assets and a badge; no library change | 43 on both |
| v0.7.4 | 2026-09-23 | OpenPBR 1.1.1 default conformance (`coat_ior` 1.6, `thin_film_ior` 1.4, `thin_film_thickness` 500 nm), `specular_weight` above 1 accepted; CMake preset, clang-tidy script, sanitizer option, deterministic floating-point flags | 43 |
| v0.7.3 | 2026-08-02 | Opacity-micromap asset format: importer, baker, `map_opacity`, mesh references; the `shaderball_checker` scene. Authorable mesh bounds (`bounds`) and `tools/obj_bounds.py` | 42 |
| v0.7.2 | 2026-07-29 | Table-driven material keyword dispatch | |
