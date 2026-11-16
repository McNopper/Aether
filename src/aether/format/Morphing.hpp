#ifndef AETHER_FORMAT_MORPHING_HPP
#define AETHER_FORMAT_MORPHING_HPP

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "aether/types/Math.hpp"
#include "aether/types/MeshData.hpp"
#include "aether/types/SceneDesc.hpp"

namespace aether {

/// ANI5 - deformed vertex data (the morph CPU reference's output): the
/// morphed positions in the mesh's OBJECT space. Normals are NOT re-derived
/// (the documented POSITION-only v1 contract - the renderer kernel matches).
/// The renderers refit their per-frame BLAS with these vertices; the instance
/// transform and the whole instance/TLAS pipeline stay exactly as authored.
struct MorphedVertices {
    std::vector<Vec3> positions;
};

/// ANI5 CPU reference - morph-target (blendshape) deformation:
///
///   p'_v = p_v + Σ_t  weight[t] * deltas[t][v]
///
/// @p morphs are the targets OF ONE MESH (scene order - filter
/// `SceneDesc::morphs` by `meshName`) and @p weights is the parallel live
/// weight vector (`SceneAnimator::evaluateMorphWeights`). A target with weight
/// 0 contributes nothing; negative or >1 weights are legal scalars (no
/// clamping - the authored weight policy).
///
/// Returns std::nullopt on a contract violation (morphs.size() !=
/// weights.size(), or any deltas.size() != mesh vertex count) - callers treat
/// that as a bind error, never as an empty deformation.
[[nodiscard]] std::optional<MorphedVertices>
deformMorphs(const MeshData& mesh, std::span<const MorphTarget> morphs, std::span<const float> weights);

/// ANI5 bind-time validation (mirrors validateSkinVertexCounts): every
/// declared morph's mesh must have been imported with exactly the vertex count
/// its deltas cover. @p vertexCounts maps mesh name -> imported vertex total
/// (the scene loader supplies its imports; tests supply synthetic maps).
/// Returns false and names the offending morph in @p error. Morph-free
/// scenes validate vacuously (true).
[[nodiscard]] bool validateMorphVertexCounts(const SceneDesc& desc,
                                             const std::unordered_map<std::string, std::size_t>& vertexCounts,
                                             std::string* error);

} // namespace aether
#endif // AETHER_FORMAT_MORPHING_HPP
