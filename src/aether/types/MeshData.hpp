#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aether/types/Math.hpp"

namespace aether {

/// A single mesh vertex in object/local space.
///
/// Aether stores only the data present in a Wavefront OBJ (position, normal,
/// uv). Tangents and the GPU-specific interleaved layout are the consumer's
/// responsibility (Harmonia builds its `GpuVertex` from this).
struct Vertex {
    Vec3 position{0.0F};
    Vec3 normal{0.0F, 1.0F, 0.0F};
    Vec2 uv{0.0F};
};

/// Indexed triangle mesh: deduplicated vertices + a flat index buffer.
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    [[nodiscard]] bool empty() const noexcept { return indices.empty(); }
};

/// One named sub-mesh from an OBJ file (split on `o` / `g` / `usemtl`).
///
/// The `name` is the OBJ group/object name; material assignment is performed by
/// the scene (via SceneDesc group/override material names) — OBJ materials are
/// never imported.
struct MeshGroup {
    std::string name;
    MeshData mesh;
};

} // namespace aether
