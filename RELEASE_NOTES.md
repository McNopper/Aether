# Release notes — Aether

Notes of each Aether release, newest first. A short history of all releases is in
[PLAN.md](PLAN.md#release-history); older details are in `git log`.

## Aether v1.0.0-pre-alpha (2026-11-28) — prerelease

Pre-alpha development snapshot and GitHub backup. Adds ACEScg/`lin_ap1_scene` material, texture,
light and working-space tokens, scene-wide camera `exposure_compensation_ev`, and the reusable
OpenPBR asset-preparation tools. The release carries the modified OpenPBR Shader Playground
Aether-format package as a separately licensed release asset; the release description records its
size, redistribution, and attribution requirements.

Verification: **206/206** CTest tests, **111** graph checks, **17** golden conversions, **5**
lat-long checks, and **6** USD-audit checks pass. This is a prerelease, not a claim that every
OpenPBR renderer or every material/camera configuration is fully conformant.

## Aether v1.0.0 (2026-11-26) — stable release

Stable public baseline of the GPU-agnostic file-format library. It coordinates with slang-math
v1.0.0 and with the Harmonia v1.0.0 family.

- Removed deterministic analyzer findings in scene parsing and skinning.
- Adopted the strict documentation-reference checks.
- The architecture check and the dead-code check report no findings.

Verification: full suite **168/168**; `verify-full`, `check_arch`, `check_dead` and
`check_docs_refs --strict` clean.

## Aether v0.12.0 (2026-11-24) — minor release

Family version-alignment release accompanying Harmonia's shared camera-assembly work. No Aether
source changes. Project version and slang-math pin are 0.12.0.

## Aether v0.9.0 (2026-11-16) — minor release

File-format library release that carries the deformation schema, consumed by Harmonia v0.8.0,
Theia v0.9.0 and Hyperion v0.8.0.

### Skinning

- `[[skin]]` block with `node`, `joints`, per-joint row-major `inverse_bind`, per-vertex
  `bone_indices` and `bone_weights`, and `blend = "lbs"` or `"dqs"` (default `"lbs"`), with full
  parser validation.
- `SceneDesc::nodeWorlds` exposes the world matrix of every node, filled by the node resolution.
- `SceneAnimator::evaluateSkins` and `evaluateSkinsAtTick` produce the object-space skin
  matrices `inverse(nodeWorld) * jointWorld * inverseBind`.
- `validateSkinVertexCounts` checks the per-vertex row counts when the scene is loaded.
- `deformSkinLbs` is the CPU reference for linear blend skinning (glTF weight normalization,
  normals blended as directions).
- `deformSkinDqs` is the CPU reference for dual-quaternion skinning (rigid, scale dropped). It
  uses the dual quaternions of `skinDualQuaternions`, which the renderer staging calls as well, so
  CPU and GPU share one extraction.

### Morph targets

- `[[morph]]` block with `mesh`, `name`, optional `weight` and per-vertex position `deltas`;
  procedural spheres are rejected at parse time.
- Weight tracks with the target `morphs/<mesh>/<name>`; `SceneAnimator::evaluateMorphWeights` and
  `evaluateMorphWeightsAtTick`.
- `deformMorphs` computes `p' = p + sum(w * delta)`; positions only.

### Blender exporter

- Version 1.4.0, deformation export: armature bones as `<armature>::<bone>` nodes, `[[skin]]`
  blocks with the four strongest same-named vertex-group weights per vertex and the chosen blend
  mode, `[[morph]]` blocks with morph weight tracks. Per-vertex rows follow the importer's
  deduplicated vertex order (6 face corners become 4 imported vertices); `tests/test_blender_roundtrip.cpp`
  checks this against the committed fixtures (commit 603042c).
- Version 1.5.0, camera bound to a node: `[camera] node = "<id>"` with `SceneAnimator::evaluateCamera`
  and `evaluateCameraAtTick`. Camera nodes are rotated instead of conjugated, so
  `forward = rotation * (0, 0, -1)` is the true aim. Example scenes `camera_orbit` and
  `skinned_orbit`, rendered with the `--frame` seek of the renderers (commit bc8e4eb).
- Version 1.6.0: pose-bone animation tracks, the scene-level render, tonemap, post-tonemap and
  metadata options, node descriptions and tags, per-sub-mesh material bindings, mesh bounds and
  link tables.

### MaterialX OpenPBR examples

The ten official MaterialX OpenPBR examples, each verified to be a flat document (no node graphs),
merged into `assets/openpbr_mtlx.materials.toml` with the showcase scene `assets/openpbr_mtlx.scene.toml`;
the converter's golden tests pass 17/17 (commit 9ee4e7a).

### Tests

`ctest`: **150/150** (new: 21 skinning tests, 19 morphing tests, 8 dual-quaternion tests, 3
blend-key tests).

### Upgrade notes

- The release is additive: existing scenes parse unchanged (`blend` defaults to `"lbs"`).
- Consumers call `validateMorphVertexCounts` and `validateSkinVertexCounts` when they load a
  scene. `deformMorphs`, `deformSkinDqs` and `skinDualQuaternions` are the reference for the GPU
  paths.
