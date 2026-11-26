# Aether v1.0.0 (2026-11-26) - major stable release

Stable public baseline for the GPU-agnostic file-format library. Coordinates with
**slang-math v1.0.0** and the Harmonia v1.0.0 family.

## Stability review
- Removed deterministic analyzer debt in scene parsing and skinning.
- Adopted strict documentation-reference checks.
- Architecture and dead-code gates remain clean.

## Verification
- Full suite: **168/168**
- `verify-full`, `check_arch`, `check_dead`, and `check_docs_refs --strict`: clean.

## Release mechanics
- Local release-preparation commits only. Push and v1.0.0 tags require explicit authorization.

---

# Aether v0.12.0 (2026-11-24) - minor release

Family version-alignment release accompanying Harmonia's shared camera-assembly work.
No Aether source changes. Project version and slang-math pin are 0.12.0.

## Release mechanics
- Local release-preparation commits. Owner screenshot review complete; push and `v0.12.0`
  tags proceed together.

---

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

## Also in this release (added after the notes assembly)

- **Blender deformation export** (blender_to_aether v1.4.0): armature bones as
  `<arm>::<bone>` NH1 nodes, `[[skin]]` (top-4 same-named vertex-group
  weights, `blend = "lbs" | "dqs"`), `[[morph]]` shape-key deltas +
  `morphs/<mesh>/<key>` weight tracks - rows expanded in ObjImporter's
  deduplicated import order (6 face corners -> 4 imported vertices), pinned by
  `tests/test_blender_roundtrip.cpp` against committed fixtures (commit 603042c).
- **ANI8 camera-from-node** (v1.5.0): `[camera] node = "<id>"` +
  `SceneAnimator::evaluateCamera(AtTick)`; camera-node locals PRE-multiplied
  (`CONV @ R`) so `forward = rotation * (0, 0, -1)` is the true aim; example
  scenes `camera_orbit` / `skinned_orbit` with the `--frame` seek chain
  (commit bc8e4eb).
- **Gathered MaterialX OpenPBR materials**: the ten official MaterialX
  `OpenPbr` examples, flat-verified (zero nodegraphs), merged into
  `assets/openpbr_mtlx.materials.toml` + the `openpbr_mtlx` showcase scene;
  converter goldens 17/17 (commit 9ee4e7a).
