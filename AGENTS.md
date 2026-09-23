# AGENTS.md â€” Aether

Quick-start context for AI agents so basic facts don't have to be rediscovered each session.

**Outstanding work, governance (definition of done + guardrails) and release history: see
[`PLAN.md`](PLAN.md).**

## What this repo is

**Aether** is the abstract **file-format library** for a four-repo rendering pipeline.
It defines scenes, materials, cameras, tonemap/render presets and the asset loaders.
It holds **GPU-agnostic** scene/material/mesh data only â€” no renderer code, no Vulkan,
no GPU-optimized layouts.

Pipeline (dependency direction):

```mermaid
flowchart LR
    SM["slang-math<br/>math"] --> A["<b>Aether</b><br/>file format (this repo)"]
    SM --> H
    A --> H["Harmonia<br/>shared Vulkan lib"]
    H --> Hy["Hyperion<br/>path tracer Â· ground truth"]
    H --> T["Theia<br/>real-time renderer"]
```

- **Hyperion** = offline path tracer, the **ground-truth** reference renderer.
- **Theia** = real-time accumulation path tracer, **converging to Hyperion**.
- Harmonia/Hyperion/Theia consume Aether via CMake **FetchContent**.

Aether must never reference the renderers. Renderer-aware tooling (e.g.
`compare_renders.py`) lives in Harmonia, not here.

## Assets

`assets/` is the canonical asset tree. `AETHER_ASSETS_DIR` (CMake) =
`${CMAKE_CURRENT_SOURCE_DIR}/assets`.

Material colors are linear Rec.709 unless the material lib says otherwise
(`colorspace = "lin_rec709_scene"`); renderers convert to the working color space.
Format is TOML (chosen as the best token/readability/comment compromise).

**Material model = OpenPBR Surface** (Academy Software Foundation). Material libraries are
tagged `model = "openpbr"` and use **OpenPBR parameter names** (`base_color`, `specular_ior`,
`transmission_weight`, `geometry_opacity`, `coat_*`, `subsurface_*`, `thin_film_*`, â€¦).
OpenPBR's canonical/reference implementation is **MaterialX** (`mx_*` nodes); when adding or
naming parameters, follow OpenPBR/MaterialX, not a renderer-specific convention. The tag exists
so future material models can coexist.

### âš ï¸ FetchContent asset gotcha (read this â€” it bites every session)

Hyperion and Theia do **NOT** read assets from this working tree. They read from their
own FetchContent clone at `<Renderer>/build/_deps/aether-src/assets/`.

So **editing `assets/*.toml` in this working tree has NO effect on a render**
unless you do one of:
- edit the copy under `<Renderer>/build/_deps/aether-src/assets/` directly, or
- configure the renderer build with `-DFETCHCONTENT_SOURCE_DIR_AETHER=<path-to-aether>`
  (then `aether-src` points at this working tree), or
- copy the edited file into the `_deps` copy before rendering.

Symptom when you forget: two "different" renders produce **byte-identical** metrics.

### Render presets & sample counts (the low-spp reference trap)

Path-traced references are only clean at high spp. The IBL scenes (openpbr_*,
shaderball_*, dragon_teapot) reference the meadow/relax presets up to **256 spp**;
`preview.render.toml` is a fast **64 spp** direct-light-only preset (no IBL, used by the
cornell scenes).

Comparing Theia (noise-free) against a low-spp IBL reference inflates `mean_diff` with
Monte-Carlo **noise**, not a real discrepancy. For any IBL parity check, render the
Hyperion reference at high spp (`hyperion --spp 256`) before drawing conclusions.

## Build & test

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
      -DCMAKE_TOOLCHAIN_FILE="<vcpkg-root>/scripts/buildsystems/vcpkg.cmake"
cmake --build build
cd build; ctest --output-on-failure
```

Equivalent preset flow (Ninja + Release + clang-cl + `$env:VCPKG_ROOT` toolchain):
`cmake --preset win` / `cmake --build --preset win` / `ctest --preset win`.

**Static analysis:** `python tools/check_tidy.py` â€” parallel clang-tidy over
`build/compile_commands.json`, classified per `.clang-tidy`'s WarningsAsErrors contract
(clang-diagnostic/clang-analyzer/bugprone fail the run; modernize/performance/portability
are report-only). Also registered as ctest `test_tidy` (label `analysis`; the fast test loop is `ctest -LE analysis`; skips when
clang-tidy, Python3 or the database is missing). Sanitizer lane (Clang/GCC configures only):
`-DAETHER_SANITIZER=address|undefined|thread`. Host-side FP is deterministic
(`/fp:strict` / `-ffp-contract=off -fno-fast-math`).

## Conventions

- Commit, but do **not** push unless asked.
- OBJ winding must be outward-facing (CCW-from-outside); inward winding renders black
  (Hyperion derives emissive-triangle normals from winding).
- Material model is **OpenPBR Surface** (tagged `model = "openpbr"`, MaterialX-defined); the tag
  exists so future models can coexist.
