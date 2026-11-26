#ifndef AETHER_TYPES_MESHDATA_HPP
#define AETHER_TYPES_MESHDATA_HPP

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

// Vertex is 8 tightly packed floats (position, normal, uv) with no padding.
// ObjImporter hashes/compares it byte-wise, which is only well-defined while
// this invariant holds, so enforce it at compile time.
static_assert(sizeof(Vertex) == 8 * sizeof(float),
              "Vertex must stay 8 tightly packed floats for byte-wise hashing/comparison.");

/// Indexed triangle mesh: deduplicated vertices + a flat index buffer.
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;

    [[nodiscard]] bool empty() const noexcept { return indices.empty(); }
};

/// One sub-mesh from an OBJ file, split at every `o` / `g` / `usemtl` statement.
///
/// A sub-mesh carries the two labels an OBJ file can give it, and the scene may
/// address it by either one (per-group material bindings):
///  - `name`: the `o` / `g` group/object name in effect, or the file stem when
///    the file declares none;
///  - `materialName`: the `usemtl` material name in effect, empty when none.
/// OBJ material definitions are never imported - material assignment is the
/// scene's job.
struct MeshGroup {
    std::string name;
    std::string materialName;
    MeshData mesh;
};

} // namespace aether
#endif // AETHER_TYPES_MESHDATA_HPP
