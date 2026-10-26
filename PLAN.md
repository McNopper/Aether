# PLAN — Aether

**Living source of truth for outstanding work.** Shipped items are removed from the work
lists; the shipped record is the *Baseline* below and `git log`. Only outstanding work is
tracked here.

**How to read this plan** — *How to continue* is the prioritized next-up list. Before
editing, read `AGENTS.md` (orientation, gotchas — especially the FetchContent asset trap),
then *How to continue* → *Governance* (definition of done + guardrails).

## Context

Aether is the GPU-agnostic file-format library of a five-repo rendering pipeline:

```mermaid
flowchart LR
    SM["slang-math<br/>math"] --> A["<b>Aether</b><br/>file format (this repo)"]
    SM --> H
    A --> H["Harmonia<br/>shared Vulkan lib + BSDF / estimator"]
    H --> Hy["Hyperion<br/>path tracer · ground truth"]
    H --> T["Theia<br/>real-time · games-grade"]
```

The family's material standard is OpenPBR: currently **1.1.1**, bumping to **1.1.2** (patch) on its release via C16 in `Harmonia/PLAN.md`; the **1.2** minor spec is a separate later track (C15). After the 1.1.2 bump, older versions are unsupported by declaration, with Theia converging to
Hyperion's unbiased ground truth. Aether carries the scene/material/mesh **data** and the authoring
tools — never renderer code, never Vulkan. Sibling plans: `Harmonia/PLAN.md`,
`Hyperion/PLAN.md`, `Theia/PLAN.md`, `slang-math/PLAN.md` in the sibling clones.

## At a glance

- **Release:** v0.7.8 (tag-synced with GitHub); consumed via FetchContent pins by
  Harmonia -> Hyperion/Theia; the slang-math pin is bumped to v0.4.0.
- **Last shipped (v0.7.8):** A-02 (one `obj_transform` CLI), A-05 (kw tables + pairing invariant), SM6 (`sm::saturate`); tests 44->45. Prior (v0.7.7): META1 `[meta]` block (agent-legible scene metadata),
  A-01 `_obj_transform` dedup (one parameterized `transform_vec3_line` + thin aliases),
  A-03 `parseMesh` guard clauses, output color-space shim removal, Q1 coverage lane
  (83.48% lines), architecture gate orphan-free.
- **Quality gates (family-uniform):** blocking lint (Q2-Q6) on every push -
  clang-format pinned 22.1.3, ruff contract in `ruff.toml` (identical x5), cppcheck,
  bandit, actionlint 1.7.12, yamllint; plus **blocking coverage CI** (`coverage (Q1)`).
- **Next:** META1 (agent-legible `[meta]` block, marked *next* in its table
  below). NH1 (node graph) and ANI1 (keyframed transform channels) are done —
  the animation track now unblocks renderer-side time sampling across the
  family.

## How to continue

The Aether items below cover scene description & representation; see the Harmonia plan for
the family-wide ordering.

### Metadata track (META) — agent-legible format

**Goal:** when an agent reads an Aether TOML it *understands* the scene without further
reading — and, crucially, can **skip opening linked files** because their essentials are
summarised inline (what the thing is, its bounds incl. orientation, and enough content
summary to reason about it). Unblocked and independent of AR1/NH1.

| ID | Task | Deps | Status |
|----|------|------|--------|
| META1 | **`[meta]` block in the scene/material TOML** — optional, backward compatible (existing scenes parse unchanged; unknown tables already ignored). `[meta]`: `schema`/`version`, `model = "openpbr"`, `title`, `description` (agent-readable prose), `units`, `generator`. **`[meta.link."<path>"]` per linked file** — `kind` (`material-library` \| `mesh` \| `env` \| `micromap` \| `scene`), `description` (e.g. *"Suzanne the monkey — glossy ceramic, this is the snow-scene hero"*), plus the skip-the-file summary: `bounds` = AABB **with frame/orientation** (`{ min, max, frame = "object", orientation = [x,y,z,w] }` — object-space AABB + the instance TRS already in the scene makes the world AABB derivable), content counts (`triangles`, `groups`, `materials = [...]`), and per-material one-liners where useful. Parser stores it (`SceneDesc.meta` / `LinkMeta` in `src/aether/types/`); generators populate it (`blender_to_aether`, `mtlx_to_aether`, `omm_bake`); nothing consumes it yet — the consumer is the agent. | — | **next** |
| META2 | **Hierarchical agent-legible metadata (node level)** — extends META1 to the NH1 graph: `[[node]]` gains `description` (what this node/subtree IS) and `tags` (categorization), and `[meta]` gains `assembly` (prose: how the scene fits together — what the parts are and how they connect). All pure AI context — absent or wrong-typed keys parse leniently and never affect the resolved flat instance list or validation (`NodeDesc`/`SceneMeta` in `src/aether/types/SceneDesc.hpp`, parsed in `SceneParser.cpp`; tests in `tests/test_scene_parser.cpp` cover landing, empty defaults, wrong-type leniency and instance-list invariance). The real consumer exists: **the NH1 node graph is the consumer — hierarchical agent legibility** (an agent reading the TOML understands each subtree and how to assemble the scene). The previous META2 scope (generator-side population + docs) remains deferred. | META1 | **done** |

Schema sketch (scene TOML):

```toml
[meta]
schema = "aether-scene"
version = 1
model = "openpbr"
title = "Suzanne in the snow"
description = "Snow scene: Suzanne (glossy ceramic) on a snowy ground; sun IBL, falling snow via opacity masks."
assembly = "The rig nodes parent the shaderball; the plane is the snowy ground; snow particles hang in a subtree above."
units = { length = "m", luminance = "nits" }
generator = "blender_to_aether/1.2.0"

[meta.link."materials/snow.toml"]
kind = "material-library"
description = "Physically based OpenPBR materials for the snow scene."
materials = ["snow", "ceramic_white"]

[meta.link."meshes/suzanne.obj"]
kind = "mesh"
description = "Suzanne the monkey - glossy ceramic, the snow-scene hero."
bounds = { min = [-0.50, -0.31, -0.42], max = [0.50, 0.48, 0.31], frame = "object", orientation = [0, 0, 0, 1] }
triangles = 2904
groups = ["default"]
```

### Node hierarchy track (NH) — Aether slice

Aether today carries flat instances with per-instance TRS (`parseTRS`). A parent/child node
graph lets a child inherit and compound its parent's transform, and enables instanced
subtrees. Architecture directive: compounding happens loader-side in Aether (NH1) - the
renderers always receive the flat, world-resolved instance list (Harmonia NH2 dropped).

| ID | Task | Deps | Status |
|----|------|------|--------|
| NH1 | **Node graph in Aether** — extend the scene TOML + `SceneParser`: parent/child nodes with compounded TRS; instances reference a node path. Today `InstanceDesc` (`src/aether/types/SceneDesc.hpp:87`) is flat with no parent field. | — | **done** |
| NH4 | **DCC round-trip** - `blender_to_aether` (done: emits the NH1 node graph, verified headless on Blender 5.2.3; also emits the `[[animation]]` tracks 1:1 (NH4b) with the ANI2 tick clock and the `[meta]` `generator` field) / `mtlx_to_aether` emit the hierarchy (Blender collections/parents → Aether nodes). Standing constraint: Blender authors no OpenPBR, and the MaterialX path rejects non-OpenPBR sources (`tools/README.md`) — hierarchy export does not change that. | NH1 | done (blender side; mtlx pending) |

### Animation track (ANI) — Aether slice

**Architecture directive (binding):** ALL animation logic — tracks,
interpolation, evaluation — lives in Aether. The renderers
(Harmonia/Theia/Hyperion) advance the clock and call the evaluation API; they
ALWAYS receive only the flat, world-resolved instance list with the animated
world transforms, and never see keyframes. The time base is deterministic and
tick-based: each scene carries its own animation fps (`[animation_clock] fps`,
required whenever tracks exist) and the renderers advance integer ticks via
`SceneAnimator::evaluateAtTick` (seconds = tick / fps) — independent of the
render rate and exactly reproducible.

| ID | Task | Deps | Status |
|----|------|------|--------|
| ANI1 | **Keyframed transform channels (schema + evaluation)** — `[[animation]]` tracks: `target = "nodes/<id>/<parameter>"` bound against the NH1 node graph, `interpolation` = step / linear / cubic_spline with glTF 2.0 sampler semantics (1:1), clamped sampling outside `[first, last]`, rotation keys normalized at load + samples renormalized (shortest-arc slerp), scale floored away from zero; validation (unknown node/parameter, non-finite values, non-ascending times, wrong arity, empty track) as parse errors. Evaluation: `SceneAnimator::evaluate(scene, t)` — sample tracks → node-local TRS → re-run the NH1 compounding → fully resolved flat instance list. **+ tick clock (`[animation_clock] fps`, `SceneAnimator::evaluateAtTick`) + scalar component targets** (`nodes/<id>/<parameter>/<component>` with per-parameter component validation; rotation components renormalize the whole quaternion; mixed component key times parse and evaluate — the mismatched-F-curve export case now works Aether-side). | NH1 | **done** |

## Governance

**Definition of done (per change):** `ctest` green **+** warning-clean build (strict
warnings-as-errors: clang-cl `/W4 /WX`, Clang/GNU `-Wall -Wextra -Werror -Wpedantic`; a
compiler warning is a build failure — fix the cause, never silence it). **Per release:** the
above plus `verify-full` (verify + format-check + clang-tidy + cppcheck).

**Guardrails:**

- Aether must never reference the renderers (AGENTS.md) — GPU-agnostic CPU data only.
- **OpenPBR Surface** is the material standard: conformance target **1.1.2** on its release (1.1-line patch; older versions unsupported by declaration; the **1.2** minor spec is a separate track); parameter naming follows OpenPBR /
  MaterialX, never a renderer-specific convention.
- Fix bugs at once — root-caused and patched immediately. Deferral is not an
  acceptable resolution for a known defect.
- Solve directly, never defer: divergences are root-caused and fixed in code.
- Tags are always pushed to GitHub — after any local tag, verify `git ls-remote --tags
  origin` == `git tag -l`. Release order follows the dependency direction (slang-math →
  **Aether** → Harmonia → Theia/Hyperion); downstream FetchContent pins bump to the fresh tag
  in their own release commits.
- Commit per-repo; push only on explicit OK.
- Living document — done work is removed from this file; the record is the Baseline and
  `git log`.

## Baseline

- **v0.7.8**: **tools + kw tables + SM6.** A-02 (one `tools/obj_transform.py` CLI, the seven per-axis scripts removed after a 13/13 byte-identity matrix), A-05 (dispatch tables hoisted to `MaterialKwTables.hpp` + pairing-invariant test), SM6 (material weight clamps via `sm::saturate`), slang-math pin v0.4.0, blocking coverage CI. 45/45 ctest, gate clean.
- **v0.7.7**: **cleanup + quality.** A-01 dedup (the identical transform
  functions in `_obj_transform` -> one parameterized `transform_vec3_line` + thin aliases),
  A-03 guard-clause flattening of the `parseMesh` bounds nesting, the Q1 coverage lane
  (`AETHER_COVERAGE` + `tools/coverage.py`, verified 83.48% lines), and the architecture gate
  (vendored check-arch engine, orphan-free). 44/44 ctest, gate clean.

- **v0.7.6**: **dead surface removed + architecture gate.**
  The `Aether.hpp` umbrella header had zero consumers in all five repos (no include, no
  CMake reference) — deleted per the only-latest-and-greatest rule. `tools/check_layers.py`
  (ctest `test_layers`, label `analysis`): the module DAG `aether/types` ← `aether/format`
  ← root, file-level cycle detection, and any `harmonia/`/`theia/`/`hyperion/` include
  fails (Aether sits below Harmonia; the sibling renderers are out of reach). 44/44 ctest
  on Windows; Linux suite green.
- **v0.7.5** (family-wide release): **dual-OS CI + badge; test
  registration for the CI ctest lane.** `.github/workflows/build.yml` on `windows-latest` +
  `ubuntu-26.04` with the Git LFS asset checkout (the family's LFS content lives in Aether); the
  CPU suite runs in CI (43/43 on both OSes); `tests/CMakeLists.txt` registers the CI ctest
  lane. **No library code changes since v0.7.4** — the parser and format are identical, so
  downstream pins may remain at v0.7.4 until the next release.
- **v0.7.4** (ships alongside the family v0.7.8): **OpenPBR 1.1.1 spec-default
  conformance** — `coat_ior` default 1.6 (was 1.5), `thin_film_ior` default 1.4 (was 1.5),
  `thin_film_thickness` default 500 nm (spec 0.5 µm; was 0) — inert for all shipped assets
  (every coated/thin-film material sets the parameters explicitly) — and the parser now
  accepts `specular_weight` > 1 (spec range [0,∞); the mandated ξ_s·F_s ≤ 1 clamp lives in
  the shader, Harmonia v0.7.8). **Tooling**: `CMakePresets.json`
  (`win` = Ninja + Release + clang-cl + `$env{VCPKG_ROOT}` toolchain),
  `tools/check_tidy.py` (parallel classified clang-tidy lane) + ctest `test_tidy` (label
  `analysis`), `AETHER_SANITIZER` option (Clang/GCC), deterministic host FP
  (`/fp:strict` / `-ffp-contract=off -fno-fast-math`). 43 ctest green at ship time (incl.
  the tidy lane).
- **v0.7.3** (shipped alongside the family v0.7.7): the **opacity-micromap asset
  format** — `aether::OpacityMicromapData`/`OpacityMicromapGroup`
  (`src/aether/types/OpacityMicromap.hpp`), the `.omm` text-sidecar + `.micromap.toml`
  descriptor importer (`OmmImporter`), `MaterialDesc::map_opacity` (data-space texture),
  `MeshDesc::opacityMicromaps` (OBJ-group → baked-asset reference), and the standalone Python
  baker `tools/omm_bake.py` (PNG decode/encode, OBJ parse, the Vulkan/glTF
  space-filling-curve microtriangle index, conservative range-sampled classification — a
  microtriangle is only ever declared uniform where the renderer's own bilinear/wrap-repeat
  sampling could not disagree). New showcase scene `shaderball_checker.scene.toml` (+materials
  + baked `.omm`/`.micromap.toml` + the checker opacity PNG). 42 ctest green at ship time.
- **BV1** (earlier): `MeshDesc::bounds` — optional authorable object-space AABB
  (`bounds = { min=[...], max=[...] }`) + `tools/obj_bounds.py` (stdlib-only OBJ→AABB,
  `--write` in-place, optional Ritter sphere). Consumers shipped downstream (Harmonia BV2,
  Theia BV3).
- **v0.7.2**: `applyKw` table-driven dispatch.
- Earlier history in `git log`.
