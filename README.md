# Aether

The scene & material file format for the [Hyperion](https://github.com/McNopper/Hyperion)
and [Theia](https://github.com/McNopper/Theia) renderers — a dependency-free C++23
library that parses `.scene` / `.mtlx` / OBJ into plain CPU data structures.

> *[Aether](https://en.wikipedia.org/wiki/Aether_(mythology)) — primordial deity of the
> bright upper air and pure light the gods breathe; the medium through which light
> travels.* Aether is the **medium**: it carries a scene's description, untied to any
> renderer or GPU.

> ⚠️ **Early stage / work in progress.**

---

## What Aether is (and isn't)

Aether is **fully independent**: it has **no Vulkan, no GPU, and no renderer
dependencies** (only GLM for vector/quaternion math). It turns text files into plain
CPU structs; consumers (Harmonia and the renderers) own all GPU upload.

| Parses | Into |
|--------|------|
| `.scene` (line-based scene description) | `aether::SceneDesc` (camera, render settings, env, tonemapper, geometry blocks with TRS + material refs, material-library references) |
| `.mtlx` (OpenPBR material library — **not** Wavefront MTL, **not** MaterialX XML) | `aether::MaterialDesc` (OpenPBR Surface v1.1 parameters + texture references) |
| Wavefront OBJ (geometry only) | `aether::MeshData` / `aether::MeshGroup` (deduplicated vertices + indices, local space) |

Geometry is returned in **object/local space**; world placement lives on the scene's
geometry blocks (TRS). Colors are kept in their declared input color space, tagged with
`MaterialColorSpace`, so the consumer performs any color-space conversion.

---

## Building

**Requirements:** CMake 3.28+, a C++23 compiler, vcpkg (provides GLM).

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
| [GLM](https://github.com/g-truc/glm) | Vector / quaternion math (the only dependency) |
| [GoogleTest](https://github.com/google/googletest) | Unit testing (tests only) |