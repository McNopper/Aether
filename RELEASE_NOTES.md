# Aether v0.9.0 (2026-11-16) - minor release

File-format library release carrying the deformation schema that Harmonia v0.8.0 / Theia v0.9.0 /
Hyperion v0.8.0 consume.

## Shipped
- **ANI4 skinning**: `[[skin]]` (node, joints, per-joint row-major `inverse_bind`,
  per-vertex `bone_indices`/`bone_weights`, **`blend = "lbs" | "dqs"`**, default LBS) with full
  parser validation; `SceneDesc::nodeWorlds` exposure from `resolveNodes`;
  `SceneAnimator::evaluateSkins(AtTick)` (LOCAL-space `inv(nodeWorld)*jointWorld*IBM`);
  `validateSkinVertexCounts` (bind-time count contract); CPU references **`deformSkinLbs`**
  (glTF renormalization, direction-blend normals) and **`deformSkinDqs`** (sign-aligned
  dual-quaternion blend - rigid contract, scale dropped - via the SHARED
  `skinDualQuaternions` extraction the renderer staging also calls, so GPU/CPU parity is by
  shared code).
- **ANI5 morph targets**: `[[morph]]` (mesh/name/optional weight/per-vertex POSITION deltas;
  spheres rejected at parse), parallel `SceneDesc::morphWeights` driven by ANI1 tracks through
  the new **`morphs/<mesh>/<name>`** grammar (dedicated arity-1 `MorphWeight` registry row; the
  `nodes/` path rejects the `weight` token by name), `evaluateMorphWeights(AtTick)`,
  **`deformMorphs`** (`p' = p + sum(w*delta)`, POSITION-only, normals untouched).

## Receipts
- `ctest`: **150/150** (+21 skinning, +19 morphing, +8 DQS, +3 blend-key), clang-format +
  check_dead clean; downstream lockstep (Harmonia 180/180, Theia 61/61, Hyperion 43/43).

## Upgrade notes
- Purely additive: existing scenes parse unchanged (default `blend = lbs`).
- Consumers: `validateMorphVertexCounts` + `validateSkinVertexCounts` run at scene load;
  `deformMorphs`/`deformSkinDqs`/`skinDualQuaternions` are the GPU-spec functions.

## Release mechanics
- Local commits only: **tag `v0.9.0` and push commits + tag together** (consumer pins move to it).
